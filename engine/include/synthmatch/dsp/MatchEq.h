#pragma once

// Match EQ: a fixed bank of 31 third-octave bells (20 Hz to 20 kHz). The
// matcher works out one gain per band so that the synth's long-term spectrum
// lines up with the reference recording. This is separate from the adjustable
// EQ node: the user doesn't set these gains by hand, only how much of the
// correction to apply.

#include "synthmatch/dsp/Biquad.h"

#include <array>
#include <cmath>
#include <span>

namespace synthmatch::dsp {

inline constexpr int kMatchEqBands = 31;
inline constexpr double kMatchEqQ = 3.0;       // wide enough for neighbouring bands to blend smoothly
inline constexpr double kMatchEqMaxDb = 18.0;  // per band

// Band centres: 1 kHz is band 17, three bands per octave.
inline double matchEqBandHz(int band) { return 1000.0 * std::exp2((band - 17) / 3.0); }

// The bank's coefficients for a set of band gains. Bands too close to half the
// sample rate, and bands with no gain, are left as pass-through.
inline std::array<BiquadCoeffs, kMatchEqBands> matchEqCoeffs(double sampleRate, std::span<const float> gainsDb,
                                                             double amount = 1.0)
{
    std::array<BiquadCoeffs, kMatchEqBands> coeffs{};
    for (int band = 0; band < kMatchEqBands; ++band) {
        const double raw = band < static_cast<int>(gainsDb.size()) ? gainsDb[static_cast<std::size_t>(band)] : 0.0;
        const double gain = std::clamp(raw, -kMatchEqMaxDb, kMatchEqMaxDb) * amount;
        const double hz = matchEqBandHz(band);
        if (std::abs(gain) > 1.0e-3 && hz < 0.45 * sampleRate)
            coeffs[static_cast<std::size_t>(band)] = BiquadCoeffs::peaking(sampleRate, hz, kMatchEqQ, gain);
    }
    return coeffs;
}

// The combined gain of the bank at a frequency, in dB.
inline double matchEqGainDbAt(const std::array<BiquadCoeffs, kMatchEqBands>& coeffs, double hz, double sampleRate)
{
    double db = 0.0;
    for (const auto& c : coeffs)
        db += c.gainDbAt(hz, sampleRate);
    return db;
}

class MatchEq {
public:
    void set(double sampleRate, std::span<const float> gainsDb, double amount)
    {
        coeffs_ = matchEqCoeffs(sampleRate, gainsDb, amount);
        active_ = false;
        for (std::size_t band = 0; band < coeffs_.size(); ++band) {
            used_[band] = coeffs_[band].b0 != 1.0 || coeffs_[band].b1 != 0.0 || coeffs_[band].a1 != 0.0;
            active_ = active_ || used_[band];
        }
    }

    bool active() const { return active_; }

    void reset()
    {
        for (auto& channel : state_)
            channel.fill(BiquadState{});
    }

    void process(float* left, float* right, int numSamples)
    {
        if (!active_)
            return;
        float* channels[2] = {left, right};
        for (int ch = 0; ch < 2; ++ch) {
            for (std::size_t band = 0; band < coeffs_.size(); ++band) {
                if (!used_[band])
                    continue;
                auto& s = state_[static_cast<std::size_t>(ch)][band];
                const auto& c = coeffs_[band];
                for (int i = 0; i < numSamples; ++i)
                    channels[ch][i] = static_cast<float>(biquadTick(s, c, channels[ch][i]));
            }
        }
    }

private:
    std::array<BiquadCoeffs, kMatchEqBands> coeffs_{};
    std::array<bool, kMatchEqBands> used_{};
    std::array<std::array<BiquadState, kMatchEqBands>, 2> state_{};
    bool active_ = false;
};

} // namespace synthmatch::dsp
