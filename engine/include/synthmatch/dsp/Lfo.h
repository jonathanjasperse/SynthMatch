#pragma once

// Low-frequency oscillator shapes. Each voice runs its own LFOs, restarted at
// note-on, so a note always sounds the same no matter when it is played.

#include <cmath>
#include <numbers>

namespace synthmatch::dsp {

enum class LfoWave : int { Sine = 0, Triangle, Saw, Square, Random, Count };

// What an LFO moves. Depth 0..1 maps to:
//   Pitch       +/- 12 * depth^2 semitones (so small depths give fine vibrato)
//   Cutoff      +/- 4 * depth octaves
//   Amp         level dips by up to depth (tremolo)
//   PulseWidth  +/- 0.45 * depth
enum class LfoTarget : int { Off = 0, Pitch, Cutoff, Amp, PulseWidth, Count };

inline constexpr double kLfoPitchSemitones = 12.0;
inline constexpr double kLfoCutoffOctaves = 4.0;
inline constexpr double kLfoPulseWidth = 0.45;

// A value in [-1, 1]. phase is in [0, 1); heldRandom is the current value of
// the Random (sample-and-hold) shape, which changes once per cycle.
inline double lfoValue(LfoWave wave, double phase, double heldRandom)
{
    switch (wave) {
    case LfoWave::Triangle:
        return phase < 0.25 ? 4.0 * phase : (phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0);
    case LfoWave::Saw:
        return 2.0 * phase - 1.0;
    case LfoWave::Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case LfoWave::Random:
        return heldRandom;
    case LfoWave::Sine:
    default:
        return std::sin(2.0 * std::numbers::pi * phase);
    }
}

} // namespace synthmatch::dsp
