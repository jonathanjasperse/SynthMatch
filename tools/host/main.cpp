// synthmatch_host: loads another synth (a VST3 plugin you own) and renders
// sounds from it to WAV files, as reference recordings for training and for
// grading the matcher.
//
//   synthmatch_host "C:/Program Files/Common Files/VST3/Arturia/Mini V4.vst3" --random 50
//
// Each sound gets a .wav and a .json saying where it came from.

#include "synthmatch/Json.h"
#include "synthmatch/Render.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Pitch.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using synthmatch::Json;

void printUsage()
{
    std::puts(
        "usage: synthmatch_host PLUGIN.vst3 [options]\n"
        "\n"
        "Renders sounds from another synth plugin (VST3) to WAV files, to use as\n"
        "references for training and for grading the matcher.\n"
        "\n"
        "What to render (pick one or more):\n"
        "  --programs        each preset the plugin exposes to hosts\n"
        "  --presets DIR     each .vstpreset file in DIR\n"
        "  --random N        N random variations of the plugin's default sound\n"
        "\n"
        "Options:\n"
        "  --list            print the plugin's presets and parameters, then exit\n"
        "  --out DIR         where to write (default: references/<plugin name>)\n"
        "  --notes 45,57     MIDI notes to play, one after another (default: 57)\n"
        "  --length SEC      how long each note is held (default: 0.8)\n"
        "  --tail SEC        audio kept after the last note (default: 1.2)\n"
        "  --rate HZ         sample rate (default: 48000)\n"
        "  --share S         random variations change this fraction of the parameters (default: 0.3)\n"
        "  --amount A        and move each by up to this much of its range (default: 0.35)\n"
        "  --only A,B,...    vary only parameters whose names contain one of these words,\n"
        "                    e.g. --only \"Osc,Filter,VCF,VCA,LFO\"\n"
        "  --seed N          random seed (default: 1)\n"
        "  --keep-unpitched  keep sounds without a clear pitch (dropped by default)\n"
        "  --help            show this message");
}

struct Options {
    juce::File plugin;
    juce::File out;
    bool list = false, programs = false, keepUnpitched = false;
    juce::File presets;
    int random = 0;
    std::vector<int> notes{57};
    double length = 0.8, tail = 1.2, rate = 48000.0, share = 0.3, amount = 0.35;
    juce::uint32 seed = 1;
    juce::StringArray only; // vary only parameters whose names contain one of these
};

// Lets the plugin finish anything it does on the message thread (loading
// samples, licence checks) before we ask it for audio.
void settle(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

bool isSafeToRandomise(const juce::AudioProcessorParameter& p, const juce::StringArray& only = {})
{
    if (!p.isAutomatable() || p.isMetaParameter())
        return false;
    const auto name = p.getName(64).toLowerCase();
    // Proxies hosts add, and controls that only mute or detune everything.
    for (const char* skip : {"midi cc", "bypass", "program", "master", "volume", "output", "tune", "transpose",
                             "polyphony", "voice", "pitch bend", "bend range", "mod wheel", "aftertouch", "glide",
                             "arp", "seq", "chord", "hold", "latch", "legato", "portamento", "octave", "keyboard"})
        if (name.contains(skip))
            return false;
    if (only.isEmpty())
        return true;
    for (const auto& fragment : only)
        if (name.contains(fragment.trim().toLowerCase()))
            return true;
    return false;
}

struct Rendered {
    synthmatch::StereoBuffer audio;
    std::vector<synthmatch::NoteEvent> notes;
};

Rendered render(juce::AudioPluginInstance& plugin, const Options& o)
{
    constexpr int block = 512;
    const int channels = std::max({2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()});
    juce::AudioBuffer<float> buffer(channels, block);
    juce::MidiBuffer midi;

    plugin.reset();
    // A little silence first: some synths need a moment after a change.
    for (int i = 0; i < 20; ++i) {
        buffer.clear();
        midi.clear();
        plugin.processBlock(buffer, midi);
    }

    Rendered r;
    double t = 0.0;
    for (const int note : o.notes) {
        r.notes.push_back({t, o.length, note, 0.85f});
        t += o.length + 0.15;
    }
    const auto total = static_cast<juce::int64>((t + o.tail) * o.rate);
    r.audio.sampleRate = o.rate;
    r.audio.left.reserve(static_cast<std::size_t>(total));
    r.audio.right.reserve(static_cast<std::size_t>(total));

    for (juce::int64 pos = 0; pos < total; pos += block) {
        const int n = static_cast<int>(std::min<juce::int64>(block, total - pos));
        buffer.clear();
        midi.clear();
        for (const auto& e : r.notes) {
            const auto on = static_cast<juce::int64>(e.startSec * o.rate);
            const auto off = static_cast<juce::int64>((e.startSec + e.durationSec) * o.rate);
            if (on >= pos && on < pos + n)
                midi.addEvent(juce::MidiMessage::noteOn(1, e.note, e.velocity), static_cast<int>(on - pos));
            if (off >= pos && off < pos + n)
                midi.addEvent(juce::MidiMessage::noteOff(1, e.note), static_cast<int>(off - pos));
        }
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), channels, n);
        plugin.processBlock(view, midi);
        const float* left = buffer.getReadPointer(0);
        const float* right = buffer.getReadPointer(plugin.getTotalNumOutputChannels() > 1 ? 1 : 0);
        r.audio.left.insert(r.audio.left.end(), left, left + n);
        r.audio.right.insert(r.audio.right.end(), right, right + n);
    }
    return r;
}

// Whether a render is worth keeping, and if not, why.
juce::String check(const Rendered& r, bool keepUnpitched)
{
    float peak = 0.0f;
    for (std::size_t i = 0; i < r.audio.numFrames(); ++i) {
        if (!std::isfinite(r.audio.left[i]) || !std::isfinite(r.audio.right[i]))
            return "broken audio";
        peak = std::max({peak, std::abs(r.audio.left[i]), std::abs(r.audio.right[i])});
    }
    if (peak < 0.001f)
        return "silent";
    if (!keepUnpitched) {
        const auto found = synthmatch::match::detectNotes(synthmatch::match::toMono(r.audio.left, r.audio.right),
                                                          r.audio.sampleRate);
        if (found.notes.empty() || found.confidence < 0.5)
            return "no clear pitch";
    }
    return {};
}

int saved = 0, dropped = 0;

void save(const Rendered& r, const Options& o, const juce::String& plugin, const juce::String& what, Json source)
{
    if (const auto why = check(r, o.keepUnpitched); why.isNotEmpty()) {
        std::printf("  skipped %s: %s\n", what.toRawUTF8(), why.toRawUTF8());
        ++dropped;
        return;
    }
    auto audio = r.audio;
    // Even out levels, a little below full scale.
    float peak = 0.0f;
    for (std::size_t i = 0; i < audio.numFrames(); ++i)
        peak = std::max({peak, std::abs(audio.left[i]), std::abs(audio.right[i])});
    const float gain = 0.7f / peak;
    for (std::size_t i = 0; i < audio.numFrames(); ++i) {
        audio.left[i] *= gain;
        audio.right[i] *= gain;
    }

    const auto base = juce::File::createLegalFileName(plugin + " - " + what).replace(" ", "_");
    const auto wav = o.out.getChildFile(base + ".wav");
    std::string error;
    if (!synthmatch::writeWav(wav.getFullPathName().toStdString(), audio, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return;
    }

    Json notes = Json::array();
    for (const auto& n : r.notes) {
        Json e = Json::object();
        e.set("note", Json(n.note));
        e.set("start_s", Json(n.startSec));
        e.set("held_s", Json(n.durationSec));
        notes.push(std::move(e));
    }
    Json info = Json::object();
    info.set("plugin", Json(plugin.toStdString()));
    info.set("sound", Json(what.toStdString()));
    info.set("source", std::move(source));
    info.set("notes", std::move(notes));
    wav.withFileExtension("json").replaceWithText(juce::String(info.dump(2)));
    std::printf("  saved %s\n", wav.getFileName().toRawUTF8());
    ++saved;
}

int fail(const juce::String& message)
{
    std::fprintf(stderr, "error: %s\n", message.toRawUTF8());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const juce::String arg(argv[i]);
        const auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String(argv[++i]) : juce::String(); };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--list") {
            o.list = true;
        } else if (arg == "--programs") {
            o.programs = true;
        } else if (arg == "--keep-unpitched") {
            o.keepUnpitched = true;
        } else if (arg == "--presets") {
            o.presets = juce::File(next());
        } else if (arg == "--random") {
            o.random = next().getIntValue();
        } else if (arg == "--out") {
            o.out = juce::File(next());
        } else if (arg == "--notes") {
            o.notes.clear();
            for (const auto& n : juce::StringArray::fromTokens(next(), ",", ""))
                o.notes.push_back(juce::jlimit(0, 127, n.trim().getIntValue()));
        } else if (arg == "--length") {
            o.length = std::max(0.05, next().getDoubleValue());
        } else if (arg == "--tail") {
            o.tail = std::max(0.0, next().getDoubleValue());
        } else if (arg == "--rate") {
            o.rate = juce::jlimit(22050.0, 192000.0, next().getDoubleValue());
        } else if (arg == "--share") {
            o.share = juce::jlimit(0.0, 1.0, next().getDoubleValue());
        } else if (arg == "--amount") {
            o.amount = juce::jlimit(0.0, 1.0, next().getDoubleValue());
        } else if (arg == "--only") {
            o.only = juce::StringArray::fromTokens(next(), ",", "");
        } else if (arg == "--seed") {
            o.seed = static_cast<juce::uint32>(next().getIntValue());
        } else if (arg.startsWith("--")) {
            return fail("unknown option " + arg + " (see --help)");
        } else {
            o.plugin = juce::File::getCurrentWorkingDirectory().getChildFile(arg);
        }
    }
    if (o.plugin == juce::File() || o.notes.empty()) {
        printUsage();
        return 2;
    }
    if (!o.plugin.exists())
        return fail(o.plugin.getFullPathName() + " not found");

    const juce::ScopedJuceInitialiser_GUI juce;
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formats.getFormats())
        if (format->fileMightContainThisPluginType(o.plugin.getFullPathName()))
            format->findAllTypesForFile(types, o.plugin.getFullPathName());
    juce::PluginDescription* instrument = nullptr;
    for (auto* t : types)
        if (t->isInstrument && instrument == nullptr)
            instrument = t;
    if (instrument == nullptr)
        return fail(types.isEmpty() ? "couldn't load " + o.plugin.getFileName() : o.plugin.getFileName() + " isn't an instrument");

    juce::String error;
    auto plugin = formats.createPluginInstance(*instrument, o.rate, 512, error);
    if (!plugin)
        return fail("couldn't start " + instrument->name + ": " + error);
    settle(1500);
    plugin->enableAllBuses();
    plugin->setNonRealtime(true);
    plugin->setPlayConfigDetails(0, std::max(2, plugin->getTotalNumOutputChannels()), o.rate, 512);
    plugin->prepareToPlay(o.rate, 512);
    settle(500);

    const auto name = instrument->name;
    std::printf("%s by %s: %d presets, %d parameters\n", name.toRawUTF8(), instrument->manufacturerName.toRawUTF8(),
                plugin->getNumPrograms(), plugin->getParameters().size());

    if (o.list) {
        for (int p = 0; p < plugin->getNumPrograms(); ++p)
            std::printf("  preset %3d  %s\n", p, plugin->getProgramName(p).toRawUTF8());
        for (auto* p : plugin->getParameters())
            std::printf("  param %4d  %-32s %s%s\n", p->getParameterIndex(), p->getName(32).toRawUTF8(),
                        p->getCurrentValueAsText().toRawUTF8(), isSafeToRandomise(*p, o.only) ? "" : "   (not randomised)");
        return 0;
    }
    if (!o.programs && o.presets == juce::File() && o.random <= 0)
        return fail("say what to render: --programs, --presets DIR or --random N (see --help)");

    if (o.out == juce::File())
        o.out = juce::File::getCurrentWorkingDirectory().getChildFile("references").getChildFile(juce::File::createLegalFileName(name));
    o.out.createDirectory();

    if (o.programs) {
        std::printf("Rendering %d presets\n", plugin->getNumPrograms());
        for (int p = 0; p < plugin->getNumPrograms(); ++p) {
            plugin->setCurrentProgram(p);
            settle(300);
            Json source = Json::object();
            source.set("kind", Json("program"));
            source.set("index", Json(p));
            save(render(*plugin, o), o, name, plugin->getProgramName(p).isEmpty() ? juce::String(p) : plugin->getProgramName(p),
                 std::move(source));
        }
    }

    if (o.presets != juce::File()) {
        auto* vst3 = plugin->getVST3Client();
        const auto files = o.presets.findChildFiles(juce::File::findFiles, true, "*.vstpreset");
        std::printf("Rendering %d .vstpreset files\n", files.size());
        for (const auto& f : files) {
            juce::MemoryBlock data;
            if (vst3 == nullptr || !f.loadFileAsData(data) || !vst3->setPreset(data)) {
                std::printf("  skipped %s: couldn't load it into %s\n", f.getFileName().toRawUTF8(), name.toRawUTF8());
                ++dropped;
                continue;
            }
            settle(300);
            Json source = Json::object();
            source.set("kind", Json("vstpreset"));
            source.set("file", Json(f.getFileName().toStdString()));
            save(render(*plugin, o), o, name, f.getFileNameWithoutExtension(), std::move(source));
        }
    }

    if (o.random > 0) {
        juce::MemoryBlock home;
        plugin->getStateInformation(home);
        std::vector<juce::AudioProcessorParameter*> params;
        for (auto* p : plugin->getParameters())
            if (isSafeToRandomise(*p, o.only))
                params.push_back(p);
        std::printf("Rendering %d random variations (%zu of %d parameters may change)\n", o.random, params.size(),
                    plugin->getParameters().size());

        juce::Random random(static_cast<juce::int64>(o.seed) * 7919);
        for (int v = 0; v < o.random; ++v) {
            plugin->setStateInformation(home.getData(), static_cast<int>(home.getSize()));
            settle(100);
            Json changed = Json::object();
            for (auto* p : params) {
                if (random.nextFloat() >= o.share)
                    continue;
                const int steps = p->getNumSteps();
                float value = p->getValue();
                if (p->isDiscrete() || steps < 32)
                    value = steps > 1 ? static_cast<float>(random.nextInt(steps)) / static_cast<float>(steps - 1) : value;
                else
                    value = juce::jlimit(0.0f, 1.0f, value + static_cast<float>(o.amount) * (2.0f * random.nextFloat() - 1.0f));
                p->setValueNotifyingHost(value);
                changed.set(p->getName(64).toStdString(), Json(p->getCurrentValueAsText().toStdString()));
            }
            settle(50);
            Json source = Json::object();
            source.set("kind", Json("random"));
            source.set("seed", Json(static_cast<int>(o.seed)));
            source.set("index", Json(v));
            source.set("changed", std::move(changed));
            save(render(*plugin, o), o, name, "random " + juce::String(v + 1).paddedLeft('0', 3), std::move(source));
        }
    }

    std::printf("Saved %d sounds to %s (%d skipped)\n", saved, o.out.getFullPathName().toRawUTF8(), dropped);
    plugin.reset();
    return 0;
}
