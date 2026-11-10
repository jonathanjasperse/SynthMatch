#pragma once

// Band-limited oscillator waveforms.
//
// A naive saw or square has instant jumps, which contain frequencies above half
// the sample rate. Those fold back down as aliasing: harsh, inharmonic tones
// that get worse at high notes. PolyBLEP fixes this cheaply by reshaping the
// two samples around each jump; PolyBLAMP does the same for the triangle's
// corners.

#include <cmath>
#include <numbers>

namespace synthmatch::dsp {

enum class Wave : int { Sine = 0, Triangle, Saw, Square, Count };

inline double wrap01(double x) { return x - std::floor(x); }

// Correction that turns a naive unit step into a band-limited one.
// t  = phase since the step, wrapped to [0, 1)
// dt = phase increment per sample (frequency / sample rate), at most 0.5
inline double blepResidual(double t, double dt)
{
    if (t < dt) { // the sample just after the step
        const double x = 1.0 - t / dt;
        return -0.5 * x * x;
    }
    if (t > 1.0 - dt) { // the sample just before the step
        const double x = 1.0 + (t - 1.0) / dt;
        return 0.5 * x * x;
    }
    return 0.0;
}

// The same for a corner (a unit change of slope): the integral of blepResidual.
inline double blampResidual(double t, double dt)
{
    if (t < dt) {
        const double x = 1.0 - t / dt;
        return x * x * x / 6.0;
    }
    if (t > 1.0 - dt) {
        const double x = 1.0 + (t - 1.0) / dt;
        return x * x * x / 6.0;
    }
    return 0.0;
}

// One sample of a waveform in [-1, 1].
// phase      in [0, 1). Sine, triangle and saw start at zero, rising.
// dt         phase increment per sample, in (0, 0.5]
// pulseWidth fraction of the cycle the square spends high (0.5 = square)
inline double oscSample(Wave wave, double phase, double dt, double pulseWidth)
{
    switch (wave) {
    case Wave::Sine:
        return std::sin(2.0 * std::numbers::pi * phase);

    case Wave::Triangle: {
        const double q = wrap01(phase + 0.25);
        const double naive = q < 0.5 ? 4.0 * q - 1.0 : 3.0 - 4.0 * q;
        // Slope changes by +8 (per cycle) at the bottom corner and -8 at the top.
        return naive + 8.0 * dt * (blampResidual(q, dt) - blampResidual(wrap01(q + 0.5), dt));
    }

    case Wave::Saw: {
        const double q = wrap01(phase + 0.5);
        // Ramps from -1 to 1, then drops by 2.
        return 2.0 * q - 1.0 - 2.0 * blepResidual(q, dt);
    }

    case Wave::Square: {
        // Rises by 2 at phase 0, falls by 2 at pulseWidth. The last term removes
        // the DC offset that appears when pulseWidth is not 0.5.
        const double naive = (phase < pulseWidth ? 1.0 : -1.0) - (2.0 * pulseWidth - 1.0);
        return naive + 2.0 * blepResidual(phase, dt) - 2.0 * blepResidual(wrap01(phase - pulseWidth), dt);
    }

    case Wave::Count:
        break;
    }
    return 0.0;
}

} // namespace synthmatch::dsp
