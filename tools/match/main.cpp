// synthmatch_match: build a preset that matches a recording, without a DAW.

#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Matcher.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace {

void printUsage()
{
    std::puts(
        "usage: synthmatch_match REFERENCE.wav [options]\n"
        "\n"
        "Listens to a recording of a synth and writes a preset that sounds like it.\n"
        "\n"
        "  --out PATH       preset file to write (default: matched.json)\n"
        "  --render PATH    also render the matched preset playing the same notes\n"
        "  --model PATH     trained encoder model (.smm) for the starting guess\n"
        "  --budget N       how many candidate sounds to try, per attempt (default: 3000)\n"
        "  --attempts N     independent searches, the closest kept (default: 1)\n"
        "  --seconds S      how much of the recording to use (default: 4)\n"
        "  --threads N      CPU threads (default: all but one)\n"
        "  --seed N         random seed (default: 1)\n"
        "  --no-eq          skip the EQ match\n"
        "  --help           show this message");
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

const char* noteName(int note)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static char buffer[8];
    std::snprintf(buffer, sizeof buffer, "%s%d", names[note % 12], note / 12 - 1);
    return buffer;
}

} // namespace

int main(int argc, char** argv)
{
    std::string referencePath, outPath = "matched.json", renderPath, modelPath;
    synthmatch::match::MatchOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--no-eq") {
            options.fitMatchEq = false;
            continue;
        }
        if (!arg.starts_with("--")) {
            if (!referencePath.empty())
                return fail("only one recording at a time");
            referencePath = arg;
            continue;
        }
        if (i + 1 >= argc)
            return fail(std::string(arg) + " needs a value (see --help)");
        const char* value = argv[++i];

        if (arg == "--out")
            outPath = value;
        else if (arg == "--render")
            renderPath = value;
        else if (arg == "--model")
            modelPath = value;
        else if (arg == "--budget")
            options.budget = std::max(100, std::atoi(value));
        else if (arg == "--seconds")
            options.maxSeconds = std::max(0.2, std::atof(value));
        else if (arg == "--threads")
            options.threads = std::atoi(value);
        else if (arg == "--seed")
            options.seed = static_cast<std::uint32_t>(std::atoi(value));
        else if (arg == "--attempts")
            options.attempts = std::clamp(std::atoi(value), 1, 16);
        else
            return fail("unknown option " + std::string(arg) + " (see --help)");
    }

    if (referencePath.empty()) {
        printUsage();
        return 2;
    }

    std::string error;
    const auto reference = synthmatch::readWav(referencePath, &error);
    if (!reference)
        return fail(error);

    std::optional<synthmatch::match::Encoder> encoder;
    if (!modelPath.empty()) {
        encoder = synthmatch::match::Encoder::load(modelPath, &error);
        if (!encoder)
            return fail(error);
        options.encoder = &*encoder;
    }

    int lastPercent = -1;
    options.onProgress = [&](const synthmatch::match::MatchProgress& p) {
        const int percent = static_cast<int>(p.fraction * 100.0);
        if (percent / 10 != lastPercent / 10) {
            std::fprintf(stderr, "  %3d%%  %-12s distance %.2f dB\n", percent, p.stage, p.distance);
            lastPercent = percent;
        }
    };

    const auto started = std::chrono::steady_clock::now();
    const auto result = synthmatch::match::matchSound(*reference, options);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!result.ok)
        return fail(result.error);

    std::printf("Notes found (pitch confidence %.0f%%):\n", result.pitchConfidence * 100.0);
    for (const auto& n : result.notes) {
        std::printf("  %-4s at %.2f s for %.2f s", noteName(n.note), n.startSec, n.durationSec);
        if (result.keyShift != 0)
            std::printf("  (detected as %s)", noteName(n.note - result.keyShift));
        std::printf("\n");
    }
    if (result.keyShift != 0)
        std::printf("Moved the notes %+d semitones, so the loudest oscillator plays them.\n", result.keyShift);
    const auto& wobble = result.modulation;
    if (wobble.vibrato.found)
        std::printf("Vibrato: %.2f Hz, +/- %.0f cents\n", wobble.vibrato.rateHz, wobble.vibrato.cents);
    else if (wobble.tremolo.found)
        std::printf("Tremolo: %.2f Hz, +/- %.1f dB\n", wobble.tremolo.rateHz, wobble.tremolo.db);
    std::printf("Distance: start %.2f dB -> search %.2f dB -> with EQ match %.2f dB%s\n", result.startDistance,
                result.searchDistance, result.distance, result.usedEncoder ? "  (started from the encoder's guess)" : "");
    std::printf("Match score: %.0f / 100   (%d sounds tried in %.1f s)\n", result.score(), result.evaluations, seconds);

    auto preset = result.preset;
    preset.name = "Matched";
    if (!synthmatch::savePreset(outPath, preset, &error))
        return fail(error);
    std::printf("Wrote %s\n", outPath.c_str());

    if (!renderPath.empty()) {
        synthmatch::RenderSettings settings;
        settings.sampleRate = result.sampleRate;
        double lastOff = 0.0;
        for (const auto& n : result.notes)
            lastOff = std::max(lastOff, n.startSec + n.durationSec);
        settings.tailSec = std::max(0.0, static_cast<double>(result.numSamples) / reference->sampleRate - lastOff);
        const auto audio = synthmatch::render(preset, result.notes, settings);
        if (!synthmatch::writeWav(renderPath, audio, &error))
            return fail(error);
        std::printf("Wrote %s\n", renderPath.c_str());
    }
    return 0;
}
