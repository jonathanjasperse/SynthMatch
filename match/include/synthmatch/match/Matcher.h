#pragma once

// The matcher: given a recording of a synth, find the preset that sounds most
// like it.
//
//   1. Work out which notes the recording plays (Pitch.h).
//   2. Make a starting guess: from the trained encoder if there is one (see
//      Encoder.h), and from simple measurements of the envelope and brightness.
//   3. Search: render candidate presets through the real engine, playing the
//      same notes, and keep moving towards the ones whose spectrogram is
//      closest to the recording (CMA-ES, see Optimizer.h).
//   4. EQ match: compare the long-term spectra of the result and the
//      recording, and store the difference as the preset's match EQ curve.
//
// Everything runs on the CPU. The result is deterministic for a given seed.

#include "synthmatch/Preset.h"
#include "synthmatch/Render.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace synthmatch::match {

class Encoder;

struct MatchProgress {
    double fraction = 0.0;  // 0..1
    double distance = 0.0;  // best so far, in dB (lower is closer)
    const char* stage = ""; // "Listening", "Searching", ...
};

struct MatchOptions {
    double maxSeconds = 4.0;  // how much of the recording to use
    int budget = 3000;        // how many candidate sounds to try, per attempt
    int threads = 0;          // 0 = one per CPU core
    std::uint32_t seed = 1;
    // Independent searches, with seeds seed, seed + 1, ...; the closest wins.
    // A second attempt often finds a much better sound than the first.
    int attempts = 1;
    bool fitMatchEq = true;
    const Encoder* encoder = nullptr; // optional trained model for the starting guess
    std::function<void(const MatchProgress&)> onProgress; // called on the calling thread
    const std::atomic<bool>* cancel = nullptr;            // set to true to stop early
};

struct MatchResult {
    bool ok = false;
    std::string error; // why not, when ok is false

    Preset preset;
    std::vector<NoteEvent> notes; // the notes found in the recording
    double sampleRate = 0.0;
    std::size_t startSample = 0;  // the part of the recording that was matched
    std::size_t numSamples = 0;
    double pitchConfidence = 0.0; // low for chords or noise

    double startDistance = 0.0;    // of the starting guess, in dB
    double searchDistance = 0.0;   // after the search, before the EQ match
    double distance = 0.0;         // final
    bool usedEncoder = false;
    int evaluations = 0;

    // A 0..100 "how close" figure for display.
    double score() const;
};

// `reference` may be mono (both channels equal) or stereo; it is mixed to mono.
MatchResult matchSound(const StereoBuffer& reference, const MatchOptions& options = {});

// The distance between a recording and a preset playing the given notes, as
// the matcher measures it. Useful for tests and for checking a result.
double presetDistance(const StereoBuffer& reference, const Preset& preset, std::span<const NoteEvent> notes);

// The encoder's input for a recording, prepared exactly as matchSound prepares
// it: the first note found, at the sample rate the search runs at. Empty when
// no note is found. Used to train on real recordings (see Train.h).
std::vector<float> encoderInputFor(const StereoBuffer& recording, double maxSeconds = 4.0);

} // namespace synthmatch::match
