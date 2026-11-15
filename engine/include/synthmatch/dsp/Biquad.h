#pragma once

// Second-order (biquad) filter sections, using the well-known "Audio EQ
// Cookbook" formulas by Robert Bristow-Johnson. Used by the EQ node and by the
// match EQ.

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace synthmatch::dsp {

struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // identity by default

    // A bell: boosts or cuts around hz. q sets the width (higher = narrower).
    static BiquadCoeffs peaking(double sampleRate, double hz, double q, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = angular(sampleRate, hz);
        const double alpha = std::sin(w0) / (2.0 * std::max(q, 0.05));
        const double cosw = std::cos(w0);
        return normalised(1.0 + alpha * a, -2.0 * cosw, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cosw,
                          1.0 - alpha / a);
    }

    // Boosts or cuts everything below hz.
    static BiquadCoeffs lowShelf(double sampleRate, double hz, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = angular(sampleRate, hz);
        const double cosw = std::cos(w0);
        const double beta = 2.0 * std::sqrt(a) * (std::sin(w0) / 2.0 * std::numbers::sqrt2);
        return normalised(a * ((a + 1.0) - (a - 1.0) * cosw + beta), 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw),
                          a * ((a + 1.0) - (a - 1.0) * cosw - beta), (a + 1.0) + (a - 1.0) * cosw + beta,
                          -2.0 * ((a - 1.0) + (a + 1.0) * cosw), (a + 1.0) + (a - 1.0) * cosw - beta);
    }

    // Boosts or cuts everything above hz.
    static BiquadCoeffs highShelf(double sampleRate, double hz, double gainDb)
    {
        const double a = std::pow(10.0, gainDb / 40.0);
        const double w0 = angular(sampleRate, hz);
        const double cosw = std::cos(w0);
        const double beta = 2.0 * std::sqrt(a) * (std::sin(w0) / 2.0 * std::numbers::sqrt2);
        return normalised(a * ((a + 1.0) + (a - 1.0) * cosw + beta), -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw),
                          a * ((a + 1.0) + (a - 1.0) * cosw - beta), (a + 1.0) - (a - 1.0) * cosw + beta,
                          2.0 * ((a - 1.0) - (a + 1.0) * cosw), (a + 1.0) - (a - 1.0) * cosw - beta);
    }

    // The filter's gain at a frequency, in dB.
    double gainDbAt(double hz, double sampleRate) const
    {
        const double w = 2.0 * std::numbers::pi * hz / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const double mag = std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
        return 20.0 * std::log10(std::max(mag, 1.0e-12));
    }

private:
    static double angular(double sampleRate, double hz)
    {
        return 2.0 * std::numbers::pi * std::clamp(hz, 10.0, 0.45 * sampleRate) / sampleRate;
    }

    static BiquadCoeffs normalised(double b0, double b1, double b2, double a0, double a1, double a2)
    {
        BiquadCoeffs c;
        c.b0 = b0 / a0;
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }
};

struct BiquadState {
    double z1 = 0.0, z2 = 0.0;
};

// Filters one sample (transposed direct form II).
inline double biquadTick(BiquadState& s, const BiquadCoeffs& c, double in)
{
    const double out = c.b0 * in + s.z1;
    s.z1 = c.b1 * in - c.a1 * out + s.z2;
    s.z2 = c.b2 * in - c.a2 * out;
    return out;
}

} // namespace synthmatch::dsp
