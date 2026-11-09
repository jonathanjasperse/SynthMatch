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
    Stepped  // integer values only (waveform selectors, switches)
};

struct ParamSpec {
    std::string_view id;    // stable key used in presets and models; never rename
    std::string_view name;  // display name
    float min;
    float max;
    float defaultValue;
    Curve curve;
    std::string_view unit;
};

namespace param {
// Order must match the table in Params.cpp. Append new parameters at the end.
enum Index : int {
    GainDb = 0,
    AmpAttackMs,
    AmpReleaseMs,
    Count
};
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

ParamValues defaultValues();

} // namespace synthmatch
