#pragma once

// What the matcher "hears": a log-mel spectrogram (how loud each frequency
// band is over time), and the long-term average spectrum used by the EQ match.

#include "synthmatch/Params.h"
#include "synthmatch/match/Fft.h"

#include <cstddef>
#include <span>
#include <vector>

namespace synthmatch::match {

struct SpectrogramConfig {
    std::size_t fftSize = 2048;
    std::size_t hop = 512;
    std::size_t numBands = 64;
    double minHz = 30.0;
    double maxHz = 16000.0; // limited to just under half the sample rate
};

struct Spectrogram {
    std::size_t numFrames = 0;
    std::size_t numBands = 0;
    std::vector<float> db;      // numFrames x numBands, band power in dB
    std::vector<float> frameDb; // numFrames, total power in dB
    float peakDb = -200.0f;     // loudest band value anywhere

    float at(std::size_t frame, std::size_t band) const { return db[frame * numBands + band]; }
};

// Builds spectrograms for one sample rate. analyze() is const and keeps no
// state, so one analyzer can be shared by several threads.
class SpectrogramAnalyzer {
public:
    SpectrogramAnalyzer(double sampleRate, const SpectrogramConfig& config = {});

    // If `power` is given it receives the raw power spectrum of every frame
    // (numFrames x numBins values), on the same scale as the band values.
    Spectrogram analyze(std::span<const float> mono, std::vector<float>* power = nullptr) const;

    std::size_t numBins() const { return config_.fftSize / 2 + 1; }

    double sampleRate() const { return sampleRate_; }
    const SpectrogramConfig& config() const { return config_; }
    double bandHz(std::size_t band) const { return centres_[band]; }

private:
    struct Band {
        std::size_t firstBin = 0;
        std::vector<float> weights;
    };

    double sampleRate_;
    SpectrogramConfig config_;
    Fft fft_;
    std::vector<float> window_;
    std::vector<Band> bands_;
    std::vector<double> centres_;
};

// How different two spectrograms are, in dB (0 = identical). Values quieter
// than `rangeDb` below the reference's loudest point are treated as silence.
// `gainDb` is added to the candidate first.
struct DistanceConfig {
    float rangeDb = 70.0f;
    float envelopeWeight = 0.5f; // weight of the overall-level-over-time term
};
double spectralDistance(const Spectrogram& reference, const Spectrogram& candidate, float gainDb = 0.0f,
                        const DistanceConfig& config = {});

// The level change, in dB, that best lines the candidate up with the reference.
float bestGainDb(const Spectrogram& reference, const Spectrogram& candidate, float rangeDb = 70.0f);

// Long-term average spectrum in the match EQ's third-octave bands: mean power
// per band in dB. Bands above half the sample rate are set to -200.
MatchEqGains thirdOctaveSpectrum(std::span<const float> mono, double sampleRate);

// A finer long-term spectrum, for drawing: mean power in dB in bands a twelfth
// of an octave wide, centred on fineSpectrumHz(i) from 20 Hz up to about
// 20 kHz. Fine enough to show single harmonics. Bands above 45% of the sample
// rate are -200.
inline constexpr int kFineSpectrumBands = 120;
double fineSpectrumHz(int band);
std::vector<float> fineSpectrum(std::span<const float> mono, double sampleRate);

// The match EQ band gains that turn `candidate` into `reference`.
// `maxDb` limits each band; bands where the reference has nothing are left alone.
MatchEqGains matchEqCurve(const MatchEqGains& referenceDb, const MatchEqGains& candidateDb, double sampleRate,
                          double maxDb = 12.0);

// Solves for the per-band filter gains whose combined response hits the target
// curve at every band centre (neighbouring bands overlap, so this isn't simply
// the target itself).
MatchEqGains fitMatchEq(const MatchEqGains& targetDb, double sampleRate);

// Mono mix of a stereo signal.
std::vector<float> toMono(std::span<const float> left, std::span<const float> right);

// Halves the sample rate (with a low-pass first, so nothing folds over).
std::vector<float> decimateBy2(std::span<const float> x);

} // namespace synthmatch::match
