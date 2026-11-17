#pragma once

// The encoder: a small neural network that listens to a note and guesses the
// preset that made it. It is trained on sounds made by this synth itself
// (random presets rendered through the engine), so no outside data is needed.
//
// Its guess is only the starting point; the search in Matcher.h then refines
// it. A better-trained encoder means a better start and a closer match in the
// same time.
//
// The network is a plain multi-layer perceptron, run by the few lines of C++
// in Encoder.cpp: no ML runtime, no GPU.

#include "synthmatch/Render.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/SearchSpace.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace synthmatch::match {

// What the encoder is given: a fixed-size summary of one note.
//  - the spectrum (40 mel bands) and the first 20 harmonics, averaged over 6
//    stretches of the held note and 4 stretches after it is released
//  - how the level and brightness wobble over time (LFOs, detune beating)
//  - how much of the sound is harmonic rather than noise
//  - the note and how long it was held
//  - the measured vibrato and tremolo (rate, depth, how steady), see Modulation.h
std::size_t encoderFeatureCount();
// `endSec` is where to stop listening (the start of the next note); negative
// means the end of the recording. `pitch` is the recording's pitch track, if
// there is one already (times relative to the start of `mono`); without it the
// pitch is tracked here.
std::vector<float> encoderFeatures(std::span<const float> mono, double sampleRate, const NoteEvent& note,
                                   double endSec = -1.0, std::span<const PitchFrame> pitch = {});

// A fully connected network: ReLU between layers, raw outputs at the end.
struct Mlp {
    struct Layer {
        std::size_t inputs = 0, outputs = 0;
        std::vector<float> weights; // outputs x inputs
        std::vector<float> biases;  // outputs
    };
    std::vector<Layer> layers;

    std::size_t inputSize() const { return layers.empty() ? 0 : layers.front().inputs; }
    std::size_t outputSize() const { return layers.empty() ? 0 : layers.back().outputs; }
    std::size_t parameterCount() const;

    // Random starting weights for the given layer sizes, e.g. {631, 512, 512, 80}.
    static Mlp create(std::span<const std::size_t> sizes, std::uint32_t seed);

    std::vector<float> forward(std::span<const float> input) const;
};

class Encoder {
public:
    Encoder() = default;
    Encoder(Mlp network, std::vector<float> inputMean, std::vector<float> inputScale);

    // A model file records which parameters it was trained to predict. Loading
    // fails with a clear message if that no longer matches this build, in which
    // case the model has to be trained again.
    static std::optional<Encoder> load(const std::filesystem::path& path, std::string* error = nullptr);
    static std::optional<Encoder> fromMemory(std::span<const unsigned char> data, std::string* error = nullptr);
    bool save(const std::filesystem::path& path, std::string* error = nullptr) const;

    // The guess, as a point in the search space (one value in 0..1 per dimension).
    std::vector<double> predict(std::span<const float> mono, double sampleRate, const NoteEvent& note,
                                double endSec = -1.0, std::span<const PitchFrame> pitch = {}) const;
    std::vector<double> predictFromFeatures(std::span<const float> features) const;

    const Mlp& network() const { return network_; }
    bool valid() const { return !network_.layers.empty(); }

private:
    Mlp network_;
    std::vector<float> inputMean_, inputScale_; // features are standardised: (x - mean) * scale
};

// Turns the network's raw outputs into a point in the search space: a sigmoid
// for continuous dimensions, the most likely choice for the others.
std::vector<double> decodeOutputs(const SearchSpace& space, std::span<const float> outputs);

} // namespace synthmatch::match
