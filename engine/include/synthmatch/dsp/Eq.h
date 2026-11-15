#pragma once

// The adjustable EQ node: low shelf, two bells and a high shelf, in stereo.

#include "synthmatch/dsp/Biquad.h"

#include <array>

namespace synthmatch::dsp {

class ParametricEq {
public:
    struct Settings {
        double lowHz = 120.0, lowGainDb = 0.0;
        double mid1Hz = 500.0, mid1GainDb = 0.0, mid1Q = 1.0;
        double mid2Hz = 3000.0, mid2GainDb = 0.0, mid2Q = 1.0;
        double highHz = 8000.0, highGainDb = 0.0;
    };

    void set(double sampleRate, const Settings& s)
    {
        coeffs_[0] = BiquadCoeffs::lowShelf(sampleRate, s.lowHz, s.lowGainDb);
        coeffs_[1] = BiquadCoeffs::peaking(sampleRate, s.mid1Hz, s.mid1Q, s.mid1GainDb);
        coeffs_[2] = BiquadCoeffs::peaking(sampleRate, s.mid2Hz, s.mid2Q, s.mid2GainDb);
        coeffs_[3] = BiquadCoeffs::highShelf(sampleRate, s.highHz, s.highGainDb);
    }

    void reset()
    {
        for (auto& channel : state_)
            channel.fill(BiquadState{});
    }

    void process(float* left, float* right, int numSamples)
    {
        float* channels[2] = {left, right};
        for (int ch = 0; ch < 2; ++ch) {
            for (std::size_t band = 0; band < coeffs_.size(); ++band) {
                auto& s = state_[static_cast<std::size_t>(ch)][band];
                const auto& c = coeffs_[band];
                for (int i = 0; i < numSamples; ++i)
                    channels[ch][i] = static_cast<float>(biquadTick(s, c, channels[ch][i]));
            }
        }
    }

    double gainDbAt(double hz, double sampleRate) const
    {
        double db = 0.0;
        for (const auto& c : coeffs_)
            db += c.gainDbAt(hz, sampleRate);
        return db;
    }

private:
    std::array<BiquadCoeffs, 4> coeffs_{};
    std::array<std::array<BiquadState, 4>, 2> state_{};
};

} // namespace synthmatch::dsp
