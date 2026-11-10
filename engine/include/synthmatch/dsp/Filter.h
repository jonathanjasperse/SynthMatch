#pragma once

// State-variable filter: 12 dB/octave low-pass, band-pass and high-pass.
//
// This is the zero-delay-feedback (trapezoidal) form described by Andrew Simper
// ("Solving the continuous SVF equations using trapezoidal integration"). It
// stays stable at any cutoff and resonance, and when the cutoff is modulated
// every sample.
//
// All three outputs have a gain of Q at the cutoff frequency, so resonance
// behaves the same way for each type.

#include <algorithm>
#include <cmath>
#include <numbers>

namespace synthmatch::dsp {

enum class FilterType : int { LowPass = 0, BandPass, HighPass, Count };

inline constexpr double kMinCutoffHz = 20.0;
inline constexpr double kMaxCutoffHz = 20000.0;
inline constexpr double kMaxCutoffRatio = 0.49; // of the sample rate
inline constexpr double kMinFilterQ = std::numbers::sqrt2 / 2.0; // Butterworth: no peak
inline constexpr double kMaxFilterQ = 20.0;                      // about +26 dB at the cutoff

// Maps the 0..1 resonance parameter to the filter's damping (1/Q). Q rises
// exponentially from 0.707 to 20, which sounds even across the range.
inline double resonanceToDamping(double resonance)
{
    const double q = kMinFilterQ * std::pow(kMaxFilterQ / kMinFilterQ, std::clamp(resonance, 0.0, 1.0));
    return 1.0 / q;
}

// The cutoff range the filter accepts at this sample rate.
inline double clampCutoff(double cutoffHz, double sampleRate)
{
    return std::clamp(cutoffHz, kMinCutoffHz, std::min(kMaxCutoffHz, kMaxCutoffRatio * sampleRate));
}

struct SvfCoeffs {
    double k = std::numbers::sqrt2; // damping, 1/Q
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;

    static SvfCoeffs make(double sampleRate, double cutoffHz, double damping)
    {
        const double g = std::tan(std::numbers::pi * clampCutoff(cutoffHz, sampleRate) / sampleRate);
        SvfCoeffs c;
        c.k = damping;
        c.a1 = 1.0 / (1.0 + g * (g + damping));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }
};

struct SvfState {
    double ic1 = 0.0, ic2 = 0.0;
};

// Filters one sample.
inline double svfTick(SvfState& s, const SvfCoeffs& c, FilterType type, double in)
{
    const double v3 = in - s.ic2;
    const double v1 = c.a1 * s.ic1 + c.a2 * v3; // band-pass
    const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3; // low-pass
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;

    switch (type) {
    case FilterType::BandPass:
        return v1;
    case FilterType::HighPass:
        return in - c.k * v1 - v2;
    case FilterType::LowPass:
    default:
        return v2;
    }
}

} // namespace synthmatch::dsp
