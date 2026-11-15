#pragma once

// Training the encoder.
//
// The training data is made on the spot: pick a random preset, render one note
// of it through the engine, measure the note (encoderFeatures), and ask the
// network to recover the preset from the measurements. Repeat a few tens of
// thousands of times. No recordings are needed, and everything runs on CPU
// cores.

#include "synthmatch/match/Encoder.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace synthmatch::match {

struct TrainOptions {
    std::size_t samples = 60000;     // notes to render
    int epochs = 30;                 // passes over them
    std::size_t hiddenSize = 512;
    int hiddenLayers = 2;
    double learningRate = 1.0e-3;
    std::size_t batchSize = 128;
    double validationFraction = 0.05; // held back to measure progress honestly
    int threads = 0;                  // 0 = one per CPU core
    std::uint32_t seed = 1;
    double sampleRate = 24000.0;      // the rate the matcher searches at
    std::function<void(const std::string&)> log;
    const std::atomic<bool>* cancel = nullptr;
};

// How well the encoder does on notes it wasn't trained on.
struct TrainReport {
    double trainLoss = 0.0;
    double validationLoss = 0.0;
    std::vector<double> epochLoss; // training loss after each epoch
    // Per search dimension, on the validation notes where it can be heard:
    // continuous dimensions report the mean error as a fraction of the range;
    // choice dimensions report the fraction guessed correctly.
    std::vector<double> dimScore;
    std::vector<std::size_t> dimCount;
    double meanError = 0.0;    // over continuous dimensions
    double meanAccuracy = 0.0; // over choice dimensions
};

struct TrainResult {
    Encoder encoder;
    TrainReport report;
    bool ok = false;
};

TrainResult trainEncoder(const TrainOptions& options);

// One training example: a random preset from the kind of sounds people
// actually make (most have one or two oscillators, most have no delay, ...),
// and a random note to play it on.
struct RandomSound {
    ParamValues params;
    NoteEvent note;
    double seconds = 0.0; // length of the rendered clip
};
RandomSound randomSound(const SearchSpace& space, std::uint32_t seed);

} // namespace synthmatch::match
