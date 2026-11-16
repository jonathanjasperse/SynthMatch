// synthmatch_train: train the encoder (the network that gives the matcher its
// starting guess), and measure how much it helps.

#include "synthmatch/Json.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Matcher.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/Train.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace synthmatch;
using namespace synthmatch::match;

void printUsage()
{
    std::puts(
        "usage: synthmatch_train [options]\n"
        "\n"
        "Trains the encoder on notes rendered by the synth itself. CPU only.\n"
        "\n"
        "  --out PATH       model file to write (default: models/encoder.smm)\n"
        "  --samples N      training notes to render (default: 60000)\n"
        "  --epochs N       passes over them (default: 30)\n"
        "  --hidden N       width of the hidden layers (default: 512)\n"
        "  --threads N      CPU threads (default: all but one)\n"
        "  --seed N         random seed (default: 1)\n"
        "  --no-detect      give the network the true note, not the one note\n"
        "                   detection finds (as matching does)\n"
        "  --no-augment     keep training notes clean (no room, noise, tilt or\n"
        "                   saturation)\n"
        "\n"
        "Learning from your grades (see tools/grade):\n"
        "  --graded DIR     a graded batch folder; repeatable. Every match you rated\n"
        "                   close becomes a training example: the real recording,\n"
        "                   and the preset you picked as its answer\n"
        "  --graded-min N   the lowest rating (1-5) that counts as close (default: 4)\n"
        "  --graded-repeat N  how many synthetic notes each one is worth (default: 20)\n"
        "\n"
        "Measuring instead of training:\n"
        "  --benchmark N    match N random sounds the model has never seen and\n"
        "                   report how close the matcher gets\n"
        "  --model PATH     model to use for the benchmark (omit to test without one)\n"
        "  --budget N       sounds the matcher may try per attempt (default: 3000)\n"
        "  --attempts N     independent searches per match, closest kept (default: 1)\n"
        "  --seed N         the matcher's random seed (default: 1)\n"
        "  --help           show this message");
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

std::optional<Json> readJson(const std::filesystem::path& path)
{
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
        return std::nullopt;
    std::string text;
    char buffer[65536];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;)
        text.append(buffer, n);
    std::fclose(f);
    return Json::parse(text);
}

// The approved matches in a graded batch: the recording's encoder input, and
// the preset the person picked for it.
int loadGraded(const std::filesystem::path& dir, int minRating, std::vector<TrainOptions::Example>& out)
{
    const auto batch = readJson(dir / "batch.json");
    const auto grades = readJson(dir / "grades.json");
    if (!batch || !grades) {
        std::fprintf(stderr, "skipped %s: needs batch.json and grades.json (grade it first)\n", dir.string().c_str());
        return 0;
    }
    const Json* gradeMap = grades->find("grades");
    const Json* items = batch->find("items");
    if (!gradeMap || !items || !items->isArray())
        return 0;

    int used = 0, close = 0;
    for (const auto& item : items->items()) {
        const Json* id = item.find("id");
        const Json* grade = id && id->isString() ? gradeMap->find(id->string()) : nullptr;
        if (!grade)
            continue;
        const Json* best = grade->find("best");
        const Json* rating = grade->find("closeness");
        if (!best || !best->isNumber() || !rating || !rating->isNumber() || rating->number() < minRating)
            continue;
        ++close;
        const Json* candidates = item.find("candidates");
        const auto pick = static_cast<std::size_t>(best->number());
        if (!candidates || !candidates->isArray() || pick >= candidates->items().size())
            continue;
        const Json* presetPath = candidates->items()[pick].find("preset");
        const Json* referencePath = item.find("reference");
        if (!presetPath || !referencePath)
            continue;

        std::string error;
        const auto preset = loadPreset(dir / presetPath->string(), &error);
        const auto audio = readWav(dir / referencePath->string(), &error);
        if (!preset || !audio)
            continue;
        auto features = encoderInputFor(*audio);
        if (features.empty())
            continue;
        out.push_back({std::move(features), preset->params});
        ++used;
    }
    std::printf("%s: %d of %d close matches usable\n", dir.string().c_str(), used, close);
    return used;
}

int benchmark(int count, const std::string& modelPath, int budget, int attempts, std::uint32_t seed, int threads)
{
    std::optional<Encoder> encoder;
    if (!modelPath.empty()) {
        std::string error;
        encoder = Encoder::load(modelPath, &error);
        if (!encoder)
            return fail(error);
    }

    const SearchSpace& space = SearchSpace::standard();
    double sumStart = 0.0, sumSearch = 0.0, sumFinal = 0.0, sumSeconds = 0.0, sumGuess = 0.0, sumOracle = 0.0;
    int matched = 0, encoderWins = 0;

    std::printf("%4s  %-5s %6s  %-12s %8s %8s %8s %8s %8s  %s\n", "#", "note", "held", "found", "oracle", "guess", "start",
                "search", "final", "time");
    for (int i = 0; i < count; ++i) {
        // Seeds far away from anything used in training.
        const RandomSound sound = randomSound(space, 4000000000u + static_cast<std::uint32_t>(i) * 7919u);
        RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.tailSec = sound.seconds - sound.note.durationSec;
        const NoteEvent notes[] = {sound.note};
        const auto reference = render(sound.params, notes, settings);

        MatchOptions options;
        options.budget = budget;
        options.attempts = attempts;
        options.seed = seed;
        options.threads = threads;
        options.encoder = encoder ? &*encoder : nullptr;

        const auto started = std::chrono::steady_clock::now();
        const auto result = matchSound(reference, options);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!result.ok) {
            std::printf("%4d  failed: %s\n", i + 1, result.error.c_str());
            continue;
        }

        // How good is the encoder's guess on its own, before any search?
        double guessDistance = 0.0;
        if (encoder) {
            Preset guess;
            const auto mono = toMono(reference.left, reference.right);
            const auto x = encoder->predict(mono, reference.sampleRate, sound.note);
            guess.params = space.toParams(x);
            guessDistance = presetDistance(reference, guess, notes);
        }

        // The oracle: the true preset playing the notes that were detected. The
        // best the search could do; if it isn't near zero, the note detection is
        // what limits the match.
        StereoBuffer section;
        section.sampleRate = reference.sampleRate;
        const auto first = static_cast<std::ptrdiff_t>(result.startSample);
        const auto last = static_cast<std::ptrdiff_t>(std::min(reference.numFrames(), result.startSample + result.numSamples));
        section.left.assign(reference.left.begin() + first, reference.left.begin() + last);
        section.right.assign(reference.right.begin() + first, reference.right.begin() + last);
        Preset truth;
        truth.params = sound.params;
        const double oracle = presetDistance(section, truth, result.notes);

        char found[32];
        std::snprintf(found, sizeof found, "%d x%zu %.2fs", result.notes.empty() ? 0 : result.notes[0].note, result.notes.size(),
                      result.notes.empty() ? 0.0 : result.notes[0].durationSec);
        std::printf("%4d  %-5d %5.2fs  %-12s %8.2f %8.2f %8.2f %8.2f %8.2f  %4.1fs%s\n", i + 1, sound.note.note,
                    sound.note.durationSec, found, oracle, guessDistance, result.startDistance, result.searchDistance,
                    result.distance, seconds, result.usedEncoder ? "  enc" : "");
        sumOracle += oracle;
        if (const char* dump = std::getenv("SYNTHMATCH_BENCH_DUMP"); dump && std::atoi(dump) == i + 1) {
            const auto mono = toMono(reference.left, reference.right);
            for (const auto& f : trackPitch(mono, reference.sampleRate))
                if (f.timeSec < 1.3)
                    std::printf("        t %.2f  hz %7.1f  conf %.2f  level %6.1f\n", f.timeSec, f.hz, f.confidence, f.levelDb);
        }
        if (std::getenv("SYNTHMATCH_BENCH_VERBOSE")) {
            const auto& p = sound.params;
            std::printf("        lfo %s %.1fHz %.2f | unison %.0f | osc2 %.2f@%+.0f osc3 %.2f@%+.0f | noise %.2f | filter %s %.0fHz "
                        "res %.2f env %.1f | sus %.2f dec %.0f rel %.0f | delay %s reverb %s\n",
                        std::string(choiceName(spec(param::Lfo1Target), p[param::Lfo1Target])).c_str(), p[param::Lfo1RateHz],
                        p[param::Lfo1Depth], p[param::UnisonVoices], p[param::Osc2Level], p[param::Osc2Coarse],
                        p[param::Osc3Level], p[param::Osc3Coarse], p[param::NoiseLevel],
                        std::string(choiceName(spec(param::FilterType), p[param::FilterType])).c_str(), p[param::FilterCutoffHz],
                        p[param::FilterResonance], p[param::FilterEnvAmount], p[param::AmpSustain], p[param::AmpDecayMs],
                        p[param::AmpReleaseMs], p[param::DelayOn] > 0.5f ? "on" : "off", p[param::ReverbOn] > 0.5f ? "on" : "off");
            for (const auto& n : result.notes)
                std::printf("        found %d at %.2f for %.2f\n", n.note, n.startSec, n.durationSec);
        }
        sumGuess += guessDistance;
        sumStart += result.startDistance;
        sumSearch += result.searchDistance;
        sumFinal += result.distance;
        sumSeconds += seconds;
        encoderWins += result.usedEncoder ? 1 : 0;
        ++matched;
    }

    if (matched > 0) {
        std::printf("\nMean over %d sounds (distance in dB, lower is closer):\n", matched);
        std::printf("  true preset      %.2f   (the best possible with the notes found)\n", sumOracle / matched);
        std::printf("  starting guess   %.2f\n", sumStart / matched);
        std::printf("  after the search %.2f\n", sumSearch / matched);
        std::printf("  with EQ match    %.2f\n", sumFinal / matched);
        std::printf("  time per match   %.1f s\n", sumSeconds / matched);
        if (encoder)
            std::printf("  the encoder's guess was the best starting point in %d of %d\n", encoderWins, matched);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string outPath = "models/encoder.smm", modelPath;
    TrainOptions options;
    int benchmarkCount = 0, budget = 3000, attempts = 1, gradedMin = 4;
    std::uint32_t matchSeed = 1;
    std::vector<std::filesystem::path> graded;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--no-detect") {
            options.detectNotes = false;
            continue;
        }
        if (arg == "--no-augment") {
            options.augment = false;
            continue;
        }
        if (i + 1 >= argc)
            return fail(std::string(arg) + " needs a value (see --help)");
        const char* value = argv[++i];

        if (arg == "--out")
            outPath = value;
        else if (arg == "--samples")
            options.samples = static_cast<std::size_t>(std::max(1000, std::atoi(value)));
        else if (arg == "--epochs")
            options.epochs = std::max(1, std::atoi(value));
        else if (arg == "--hidden")
            options.hiddenSize = static_cast<std::size_t>(std::max(16, std::atoi(value)));
        else if (arg == "--threads")
            options.threads = std::atoi(value);
        else if (arg == "--seed")
            options.seed = matchSeed = static_cast<std::uint32_t>(std::atoi(value));
        else if (arg == "--attempts")
            attempts = std::clamp(std::atoi(value), 1, 16);
        else if (arg == "--benchmark")
            benchmarkCount = std::max(1, std::atoi(value));
        else if (arg == "--model")
            modelPath = value;
        else if (arg == "--budget")
            budget = std::max(100, std::atoi(value));
        else if (arg == "--graded")
            graded.emplace_back(value);
        else if (arg == "--graded-min")
            gradedMin = std::clamp(std::atoi(value), 1, 5);
        else if (arg == "--graded-repeat")
            options.extraRepeat = std::max(1, std::atoi(value));
        else
            return fail("unknown option " + std::string(arg) + " (see --help)");
    }

    for (const auto& dir : graded)
        loadGraded(dir, gradedMin, options.extra);

    if (benchmarkCount > 0)
        return benchmark(benchmarkCount, modelPath, budget, attempts, matchSeed, options.threads);

    options.log = [](const std::string& line) {
        std::puts(line.c_str());
        std::fflush(stdout);
    };

    const auto result = trainEncoder(options);
    if (!result.ok)
        return fail("training did not finish");

    const SearchSpace& space = SearchSpace::standard();
    std::puts("\nOn notes held back from training:");
    std::printf("  %-20s %s\n", "parameter", "result");
    for (std::size_t d = 0; d < space.dims.size(); ++d) {
        const auto id = std::string(spec(space.dims[d].param).id);
        if (result.report.dimCount[d] == 0)
            continue;
        if (space.dims[d].isChoice())
            std::printf("  %-20s right %3.0f%% of the time (chance: %.0f%%)\n", id.c_str(), result.report.dimScore[d] * 100.0,
                        100.0 / static_cast<double>(space.dims[d].choices.size()));
        else
            std::printf("  %-20s off by %4.1f%% of its range (a blind guess: about 25%%)\n", id.c_str(),
                        result.report.dimScore[d] * 100.0);
    }
    std::printf("\nOverall: choices right %.0f%% of the time, continuous settings off by %.1f%% of their range.\n",
                result.report.meanAccuracy * 100.0, result.report.meanError * 100.0);

    std::string error;
    const std::filesystem::path path(outPath);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    if (!result.encoder.save(path, &error))
        return fail(error);
    std::printf("Wrote %s\n", outPath.c_str());
    return 0;
}
