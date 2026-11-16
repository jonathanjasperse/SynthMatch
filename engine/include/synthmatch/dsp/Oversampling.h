#pragma once

// Two-times oversampling for the oscillators.
//
// Even band-limited (PolyBLEP) oscillators leave some aliasing: harmonics
// above half the sample rate fold back as quiet, inharmonic tones, worst on
// high notes and audible under vibrato, where they slide the wrong way.
// Running the oscillators at twice the sample rate and filtering back down
// leaves about 20 dB less (a saw's aliasing at middle C goes from -38 to
// -59 dB at 48 kHz). The filter is a linear-phase low-pass that keeps
// everything up to about 92% of the output's Nyquist frequency (-0.7 dB at
// 20 kHz at 48 kHz) and removes what lies above it before it can fold back.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace synthmatch::dsp {

inline constexpr int kDecimatorTaps = 63;

// The decimation filter: a Kaiser-windowed sinc, computed once. Call it
// before real-time use (Engine::prepare does) so the first call isn't on the
// audio thread.
inline const std::array<double, kDecimatorTaps>& decimatorTaps()
{
    static const auto taps = [] {
        constexpr double cutoff = 0.23; // of the oversampled rate (0.46 of the output's)
        constexpr double beta = 9.0;    // the Kaiser window's shape: about 90 dB stopband
        const auto besselI0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        std::array<double, kDecimatorTaps> h{};
        const double mid = 0.5 * (kDecimatorTaps - 1);
        double total = 0.0;
        for (int n = 0; n < kDecimatorTaps; ++n) {
            const double m = n - mid;
            const double x = std::numbers::pi * 2.0 * cutoff * m;
            const double sinc = m == 0.0 ? 1.0 : std::sin(x) / x;
            const double r = m / mid;
            const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
            h[static_cast<std::size_t>(n)] = sinc * window;
            total += h[static_cast<std::size_t>(n)];
        }
        for (auto& v : h)
            v /= total; // unity gain at DC
        return h;
    }();
    return taps;
}

// Takes two samples at the oversampled rate and returns one at the normal rate.
struct Decimator {
    // Each sample is stored twice, half a buffer apart, so the newest
    // kDecimatorTaps samples are always contiguous.
    std::array<double, 2 * kDecimatorTaps> history{};
    int pos = 0;

    double process(double first, double second, const std::array<double, kDecimatorTaps>& taps)
    {
        push(first);
        push(second);
        const double* x = history.data() + pos;
        double sum = 0.0;
        for (int k = 0; k < kDecimatorTaps; ++k)
            sum += taps[static_cast<std::size_t>(k)] * x[k];
        return sum;
    }

private:
    void push(double x)
    {
        history[static_cast<std::size_t>(pos)] = x;
        history[static_cast<std::size_t>(pos + kDecimatorTaps)] = x;
        if (++pos == kDecimatorTaps)
            pos = 0;
    }
};

} // namespace synthmatch::dsp
