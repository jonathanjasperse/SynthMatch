// synthmatch_render: render notes through the engine to a WAV file, without a DAW.

#include "synthmatch/Params.h"
#include "synthmatch/Preset.h"
#include "synthmatch/Render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

void printUsage()
{
    std::puts(
        "usage: synthmatch_render [options]\n"
        "\n"
        "  --out PATH        output .wav file (default: render.wav)\n"
        "  --note N          MIDI note, 0-127 (default: 60); repeat for a chord\n"
        "  --velocity V      0-1 (default: 0.8)\n"
        "  --length SEC      time until note-off (default: 1.0)\n"
        "  --tail SEC        audio rendered after note-off (default: 1.0)\n"
        "  --rate HZ         sample rate (default: 48000)\n"
        "  --tempo BPM       tempo for tempo-synced delay (default: 120)\n"
        "  --preset PATH     start from a preset file (.json)\n"
        "  --save-preset PATH  write the resulting preset to a file\n"
        "  --set ID=VALUE    set a parameter; repeatable (see --list-params)\n"
        "  --list-params     print the parameter schema and exit\n"
        "  --help            show this message");
}

void listParams()
{
    std::printf("%-18s %-18s %8s %8s %8s  %s\n", "id", "name", "min", "max", "default", "unit / choices");
    for (const auto& s : synthmatch::schema()) {
        std::string extra(s.unit);
        for (std::size_t i = 0; i < s.choices.size(); ++i)
            extra += (i == 0 ? "" : " | ") + std::string(s.choices[i]);

        std::printf("%-18.*s %-18.*s %8g %8g %8g  %s\n", static_cast<int>(s.id.size()), s.id.data(),
                    static_cast<int>(s.name.size()), s.name.data(), s.min, s.max, s.defaultValue, extra.c_str());
    }
}

bool parseDouble(const char* text, double& out)
{
    char* end = nullptr;
    out = std::strtod(text, &end);
    return end != text && *end == '\0' && std::isfinite(out);
}

bool parseInt(const char* text, int& out)
{
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0')
        return false;
    out = static_cast<int>(v);
    return true;
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::string outPath = "render.wav";
    std::vector<int> notes;
    double velocity = 0.8;
    double length = 1.0;
    synthmatch::RenderSettings settings;
    synthmatch::Preset preset;
    auto& params = preset.params;
    std::string savePresetPath;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--list-params") {
            listParams();
            return 0;
        }

        if (i + 1 >= argc)
            return fail(std::string(arg) + " needs a value (see --help)");
        const char* value = argv[++i];

        if (arg == "--out") {
            outPath = value;
        } else if (arg == "--note") {
            int n = 0;
            if (!parseInt(value, n) || n < 0 || n > 127)
                return fail("--note must be 0-127");
            notes.push_back(n);
        } else if (arg == "--velocity") {
            if (!parseDouble(value, velocity) || velocity <= 0.0 || velocity > 1.0)
                return fail("--velocity must be in (0, 1]");
        } else if (arg == "--length") {
            if (!parseDouble(value, length) || length <= 0.0)
                return fail("--length must be > 0");
        } else if (arg == "--tail") {
            if (!parseDouble(value, settings.tailSec) || settings.tailSec < 0.0)
                return fail("--tail must be >= 0");
        } else if (arg == "--rate") {
            if (!parseDouble(value, settings.sampleRate) || settings.sampleRate < 8000.0 ||
                settings.sampleRate > 384000.0)
                return fail("--rate must be 8000-384000");
        } else if (arg == "--tempo") {
            if (!parseDouble(value, settings.tempoBpm) || settings.tempoBpm < 20.0 || settings.tempoBpm > 400.0)
                return fail("--tempo must be 20-400");
        } else if (arg == "--preset") {
            std::string error;
            std::vector<std::string> warnings;
            const auto loaded = synthmatch::loadPreset(value, &error, &warnings);
            if (!loaded)
                return fail(error);
            for (const auto& w : warnings)
                std::fprintf(stderr, "warning: %s\n", w.c_str());
            preset = *loaded;
        } else if (arg == "--save-preset") {
            savePresetPath = value;
        } else if (arg == "--set") {
            const std::string_view kv = value;
            const auto eq = kv.find('=');
            if (eq == std::string_view::npos)
                return fail("--set expects ID=VALUE");
            const auto index = synthmatch::findParam(kv.substr(0, eq));
            if (!index)
                return fail("unknown parameter '" + std::string(kv.substr(0, eq)) + "' (see --list-params)");
            const auto& s = synthmatch::spec(*index);
            double v = 0.0;
            const std::string text(kv.substr(eq + 1));
            if (const auto choice = synthmatch::parseChoice(s, text))
                v = *choice; // a named choice such as osc1_wave=square
            else if (!parseDouble(text.c_str(), v))
                return fail("bad value in --set " + std::string(kv));
            if (v < s.min || v > s.max)
                std::fprintf(stderr, "warning: %s clamped to [%g, %g]\n", std::string(s.id).c_str(), s.min, s.max);
            params[static_cast<std::size_t>(*index)] = synthmatch::clampToRange(s, static_cast<float>(v));
        } else {
            return fail("unknown option " + std::string(arg) + " (see --help)");
        }
    }

    if (notes.empty())
        notes.push_back(60);

    std::vector<synthmatch::NoteEvent> events;
    for (const int n : notes)
        events.push_back({0.0, length, n, static_cast<float>(velocity)});

    const auto audio = synthmatch::render(preset, events, settings);

    std::string error;
    if (!savePresetPath.empty() && !synthmatch::savePreset(savePresetPath, preset, &error))
        return fail(error);
    if (!synthmatch::writeWav(outPath, audio, &error))
        return fail(error);

    float peak = 0.0f;
    for (std::size_t i = 0; i < audio.numFrames(); ++i)
        peak = std::max({peak, std::abs(audio.left[i]), std::abs(audio.right[i])});
    const double peakDb = peak > 0.0f ? 20.0 * std::log10(peak) : -INFINITY;

    std::printf("Wrote %s: %.2f s at %.0f Hz, peak %.1f dBFS\n", outPath.c_str(),
                static_cast<double>(audio.numFrames()) / audio.sampleRate, audio.sampleRate, peakDb);
    return 0;
}
