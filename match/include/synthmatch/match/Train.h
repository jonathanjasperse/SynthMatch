#pragma once

// Training the encoder.
//
// The training data is made on the spot: pick a random preset, render one note
// of it through the engine, measure the note (encoderFeatures), and ask the
// network to recover the preset from the measurements. Repeat a few tens of
// thousands of times. No recordings are needed (graded ones can be added, see
// `extra`), and everything runs on CPU cores.

#include "synthmatch/match/Encoder.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
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
    int threads = 0;                  // 0 = all CPU cores but one
    std::uint32_t seed = 1;
    double sampleRate = 24000.0;      // the rate the matcher searches at
    // Prepare each note's input the way matching prepares a recording: find
    // the notes first and listen to the one found (which can be off a little),
    // instead of the true note. Held-back validation notes use the true note,
    // so the validation loss doesn't include note-detection mistakes and
    // real matching does a little worse than it suggests.
    bool detectNotes = true;
    // Make most training notes sound more like recordings: now and then a
    // gentle EQ tilt, a little saturation, a small room or a noise floor. The
    // answer stays the same, so the network learns to hear past them.
    // Validation notes stay clean.
    bool augment = true;

    // Real recordings with an approved answer: the encoder's input for the
    // recording (from encoderInputFor) and the preset that matched it well,
    // as graded by a person. Each is repeated `extraRepeat` times, because
    // there are few of them and they are worth more than synthetic examples.
    struct Example {
        std::vector<float> features;
        ParamValues params;
    };
    std::vector<Example> extra;
    int extraRepeat = 20;

    // Rendering the notes takes far longer than training on them. A run with
    // `saveData` writes the rendered notes (with the graded examples and the
    // held-back ones) before training; a run with `loadData` trains on such a
    // file instead of rendering, so one render can train several networks.
    // Loading ignores `samples`, `extra`, `detectNotes` and `augment`: they
    // come from the run that saved the file. A file made for another search
    // space or other encoder inputs is refused.
    std::filesystem::path saveData;
    std::filesystem::path loadData;

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
