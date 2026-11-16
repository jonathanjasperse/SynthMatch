// synthmatch_batch: matches a set of reference recordings several times each and
// writes everything needed to grade the results by ear (tools/grade).
//
//   synthmatch_batch --refs references/MiniV4 --out batches/minimoog-1
//   synthmatch_batch --random 20 --out batches/own-1
//
// For each reference it writes the part that was matched (reference.wav) and
// several candidate matches from different starting points (a.wav, b.wav,
// c.wav, each with its preset .json), plus batch.json describing it all.

#include "synthmatch/Json.h"
#include "synthmatch/Preset.h"
#include "synthmatch/Render.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Matcher.h"
#include "synthmatch/match/Train.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace synthmatch;
using namespace synthmatch::match;

void printUsage()
{
    std::puts(
        "usage: synthmatch_batch (--refs DIR | --random N) --out DIR [options]\n"
        "\n"
        "Matches each reference several times and writes a batch to grade by ear:\n"
        "  python tools/grade/serve.py DIR\n"
        "\n"
        "  --refs DIR        a folder of .wav recordings (repeatable); for example the\n"
        "                    output of synthmatch_host, or bounces from your DAW\n"
        "  --random N        N random sounds made by SynthMatch itself\n"
        "  --out DIR         where to write the batch\n"
        "  --candidates K    matches per reference, from different starting points (default: 3, max 6)\n"
        "  --budget N        sounds tried per match (default: 6000)\n"
        "  --model PATH      trained encoder for the starting guess (default: models/encoder.smm if present)\n"
        "  --limit N         use at most N references\n"
        "  --seed N          random seed (default: 1)\n"
        "  --help            show this message");
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

std::string noteName(int note)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}

struct Reference {
    std::string source;          // file path, or "random N"
    StereoBuffer audio;
    std::optional<Preset> truth; // known for SynthMatch's own random sounds
};

} // namespace

int main(int argc, char** argv)
{
    std::vector<fs::path> refDirs;
    fs::path out, modelPath;
    int random = 0, candidates = 3, budget = 6000, limit = 0;
    std::uint32_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (i + 1 >= argc)
            return fail(std::string(arg) + " needs a value (see --help)");
        const char* value = argv[++i];
        if (arg == "--refs")
            refDirs.emplace_back(value);
        else if (arg == "--random")
            random = std::max(0, std::atoi(value));
        else if (arg == "--out")
            out = value;
        else if (arg == "--candidates")
            candidates = std::clamp(std::atoi(value), 1, 6);
        else if (arg == "--budget")
            budget = std::max(200, std::atoi(value));
        else if (arg == "--model")
            modelPath = value;
        else if (arg == "--limit")
            limit = std::max(0, std::atoi(value));
        else if (arg == "--seed")
            seed = static_cast<std::uint32_t>(std::atoi(value));
        else
            return fail("unknown option " + std::string(arg) + " (see --help)");
    }
    if (out.empty() || (refDirs.empty() && random == 0)) {
        printUsage();
        return 2;
    }

    // The references.
    std::vector<Reference> refs;
    for (const auto& dir : refDirs) {
        if (!fs::is_directory(dir))
            return fail(dir.string() + " is not a folder");
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(dir))
            if (entry.is_regular_file() && entry.path().extension() == ".wav")
                files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::string error;
            if (auto audio = readWav(f, &error))
                refs.push_back({f.string(), std::move(*audio), std::nullopt});
            else
                std::fprintf(stderr, "skipped %s: %s\n", f.string().c_str(), error.c_str());
        }
    }
    for (int r = 0; r < random; ++r) {
        const auto sound = randomSound(SearchSpace::standard(), 3000000000u + seed * 104729u + static_cast<std::uint32_t>(r));
        RenderSettings settings;
        settings.tailSec = sound.seconds - sound.note.durationSec;
        const NoteEvent notes[] = {sound.note};
        Preset truth;
        truth.name = "random " + std::to_string(r + 1);
        truth.params = sound.params;
        refs.push_back({truth.name, render(truth, notes, settings), truth});
    }
    if (limit > 0 && static_cast<int>(refs.size()) > limit)
        refs.resize(static_cast<std::size_t>(limit));
    if (refs.empty())
        return fail("no references found");

    std::optional<Encoder> encoder;
    if (modelPath.empty() && fs::exists("models/encoder.smm"))
        modelPath = "models/encoder.smm";
    if (!modelPath.empty()) {
        std::string error;
        encoder = Encoder::load(modelPath, &error);
        if (!encoder)
            return fail(error);
    }

    fs::create_directories(out / "items");
    std::printf("Matching %zu references, %d times each (about %.0f minutes)\n", refs.size(), candidates,
                static_cast<double>(refs.size()) * candidates * budget / 2000.0 / 60.0 + 0.5);

    Json items = Json::array();
    for (std::size_t r = 0; r < refs.size(); ++r) {
        const auto& ref = refs[r];
        char id[16];
        std::snprintf(id, sizeof id, "%03zu", r + 1);
        const fs::path dir = out / "items" / id;
        fs::create_directories(dir);

        Json cands = Json::array();
        std::optional<MatchResult> first;
        for (int k = 0; k < candidates; ++k) {
            MatchOptions options;
            options.budget = budget;
            options.seed = seed * 1000u + static_cast<std::uint32_t>(k) * 17u + 1u;
            options.encoder = encoder ? &*encoder : nullptr;
            auto result = matchSound(ref.audio, options);
            if (!result.ok) {
                std::printf("  %s: %s\n", id, result.error.c_str());
                break;
            }
            if (!first)
                first = result;

            // The candidate playing the notes that were found, as long as the matched part.
            RenderSettings settings;
            settings.sampleRate = result.sampleRate;
            double lastOff = 0.0;
            for (const auto& n : result.notes)
                lastOff = std::max(lastOff, n.startSec + n.durationSec);
            settings.tailSec =
                std::max(0.0, static_cast<double>(result.numSamples) / ref.audio.sampleRate - lastOff);
            const auto audio = render(result.preset, result.notes, settings);

            const std::string letter(1, static_cast<char>('a' + k));
            std::string error;
            writeWav(dir / (letter + ".wav"), audio, &error);
            result.preset.name = std::string(id) + letter;
            savePreset(dir / (letter + ".json"), result.preset, &error);

            Json c = Json::object();
            c.set("audio", Json("items/" + std::string(id) + "/" + letter + ".wav"));
            c.set("preset", Json("items/" + std::string(id) + "/" + letter + ".json"));
            c.set("distance", Json(std::round(result.distance * 100.0) / 100.0));
            c.set("score", Json(static_cast<int>(std::lround(result.score()))));
            c.set("seed", Json(static_cast<int>(options.seed)));
            cands.push(std::move(c));
        }
        if (!first)
            continue;

        // The part of the reference that was matched.
        StereoBuffer section;
        section.sampleRate = ref.audio.sampleRate;
        const auto from = static_cast<std::ptrdiff_t>(first->startSample);
        const auto to = static_cast<std::ptrdiff_t>(std::min(ref.audio.numFrames(), first->startSample + first->numSamples));
        section.left.assign(ref.audio.left.begin() + from, ref.audio.left.begin() + to);
        section.right.assign(ref.audio.right.begin() + from, ref.audio.right.begin() + to);
        std::string error;
        writeWav(dir / "reference.wav", section, &error);

        std::string notes;
        for (const auto& n : first->notes)
            notes += (notes.empty() ? "" : " ") + noteName(n.note);

        Json item = Json::object();
        item.set("id", Json(std::string(id)));
        item.set("source", Json(ref.source));
        item.set("reference", Json("items/" + std::string(id) + "/reference.wav"));
        item.set("notes", Json(notes));
        if (ref.truth) {
            savePreset(dir / "truth.json", *ref.truth, &error);
            item.set("truth", Json("items/" + std::string(id) + "/truth.json"));
        }
        item.set("candidates", std::move(cands));
        items.push(std::move(item));
        std::printf("  %s  %-6s best score %d  (%s)\n", id, notes.c_str(), static_cast<int>(std::lround(first->score())),
                    fs::path(ref.source).filename().string().c_str());
    }

    Json batch = Json::object();
    batch.set("format", Json("synthmatch-batch"));
    batch.set("version", Json(1));
    batch.set("budget", Json(budget));
    batch.set("model", Json(modelPath.empty() ? std::string("none") : modelPath.string()));
    batch.set("items", std::move(items));
    {
        std::FILE* f = std::fopen((out / "batch.json").string().c_str(), "wb");
        if (!f)
            return fail("cannot write batch.json");
        const auto text = batch.dump(2) + "\n";
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    std::printf("Wrote %s. Grade it with:  python tools/grade/serve.py \"%s\"\n", (out / "batch.json").string().c_str(),
                out.string().c_str());
    return 0;
}
