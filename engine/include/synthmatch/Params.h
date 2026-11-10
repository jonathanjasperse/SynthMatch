#pragma once

// The parameter schema: the single source of truth for every synth parameter.
// The plugin, the render CLI, preset files, the dataset generator and the ML
// normalization all read this table, so a parameter is defined exactly once.

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace synthmatch {

enum class Curve {
    Linear,  // evenly spaced between min and max
    Log,     // exponential mapping; min must be > 0 (times, frequencies)
    Stepped  // integer values only (waveform selectors, semitones)
};

struct ParamSpec {
    std::string_view id;    // stable key used in presets and models; never rename
    std::string_view name;  // display name
    float min;
    float max;
    float defaultValue;
    Curve curve;
    std::string_view unit;
    // Names for a stepped selector, one per integer value starting at min.
    // Empty for ordinary parameters.
    std::span<const std::string_view> choices{};
};

namespace param {

// Order must match the table in Params.cpp. Append new parameters at the end.
enum Index : int {
    GainDb = 0,
    AmpAttackMs,
    AmpReleaseMs,
    AmpDecayMs,
    AmpSustain,

    Osc1Wave,
    Osc1Level,
    Osc1Coarse,
    Osc1Fine,
    Osc1PulseWidth,

    Osc2Wave,
    Osc2Level,
    Osc2Coarse,
    Osc2Fine,
    Osc2PulseWidth,

    Osc3Wave,
    Osc3Level,
    Osc3Coarse,
    Osc3Fine,
    Osc3PulseWidth,

    NoiseLevel,

    Count
};

// The three oscillators have identical parameter blocks.
inline constexpr int kNumOscs = 3;
inline constexpr int kOscStride = Osc2Wave - Osc1Wave;
static_assert(Osc3Wave - Osc2Wave == kOscStride);

enum OscField : int { OscWave = 0, OscLevel, OscCoarse, OscFine, OscPulseWidth };

constexpr int osc(int oscIndex, OscField field) { return Osc1Wave + oscIndex * kOscStride + field; }

} // namespace param

inline constexpr int kNumParams = param::Count;

using ParamValues = std::array<float, kNumParams>;

std::span<const ParamSpec> schema();
const ParamSpec& spec(int index);
std::optional<int> findParam(std::string_view id);

// Clamp to [min, max] and round stepped parameters.
float clampToRange(const ParamSpec& s, float value);

// Map between real values and [0, 1] using the parameter's curve.
// Used by the plugin's host-facing ranges and by ML normalization.
float toNormalized(const ParamSpec& s, float value);
float fromNormalized(const ParamSpec& s, float normalized);

// For parameters with named choices ("saw", "square", ...).
std::string_view choiceName(const ParamSpec& s, float value); // empty if none
std::optional<float> parseChoice(const ParamSpec& s, std::string_view name);

ParamValues defaultValues();

} // namespace synthmatch
