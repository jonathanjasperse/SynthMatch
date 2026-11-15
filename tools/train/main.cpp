// synthmatch_train: train the encoder (the network that gives the matcher its
// starting guess), and measure how much it helps.

#include "synthmatch/match/Features.h"
#include "synthmatch/match/Matcher.h"
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
        "\n"
        "Measuring instead of training:\n"
        "  --benchmark N    match N random sounds the model has never seen and\n"
        "                   report how close the matcher gets\n"
        "  --model PATH     model to use for the benchmark (omit to test without one)\n"
        "  --budget N       sounds the matcher may try per match (default: 3000)\n"
        "  --help           show this message");
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

int benchmark(int count, const std::string& modelPath, int budget, int threads)
{
    std::optional<Encoder> encoder;
    if (!modelPath.empty()) {
        std::string error;
        encoder = Encoder::load(modelPath, &error);
        if (!encoder)
            return fail(error);
    }

    const SearchSpace& space = SearchSpace::standard();
    double sumStart = 0.0, sumSearch = 0.0, sumFinal = 0.0, sumSeconds = 0.0, sumGuess = 0.0;
    int matched = 0, encoderWins = 0;

    std::printf("%4s  %-5s %6s  %8s %8s %8s %8s  %s\n", "#", "note", "held", "guess", "start", "search", "final", "time");
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

        std::printf("%4d  %-5d %5.2fs  %8.2f %8.2f %8.2f %8.2f  %4.1fs%s\n", i + 1, sound.note.note,
                    sound.note.durationSec, guessDistance, result.startDistance, result.searchDistance, result.distance,
                    seconds, result.usedEncoder ? "  enc" : "");
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
    int benchmarkCount = 0, budget = 3000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
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
            options.seed = static_cast<std::uint32_t>(std::atoi(value));
        else if (arg == "--benchmark")
            benchmarkCount = std::max(1, std::atoi(value));
        else if (arg == "--model")
            modelPath = value;
        else if (arg == "--budget")
            budget = std::max(100, std::atoi(value));
        else
            return fail("unknown option " + std::string(arg) + " (see --help)");
    }

    if (benchmarkCount > 0)
        return benchmark(benchmarkCount, modelPath, budget, options.threads);

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
