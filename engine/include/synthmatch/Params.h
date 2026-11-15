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

    FilterType,
    FilterCutoffHz,
    FilterResonance,
    FilterKeyTrack,
    FilterEnvAmount,
    FilterAttackMs,
    FilterDecayMs,
    FilterSustain,
    FilterReleaseMs,

    UnisonVoices,
    UnisonDetune,
    UnisonSpread,

    Lfo1Wave,
    Lfo1RateHz,
    Lfo1Target,
    Lfo1Depth,

    Lfo2Wave,
    Lfo2RateHz,
    Lfo2Target,
    Lfo2Depth,

    BendRange,

    EqOn,
    EqLowFreqHz,
    EqLowGainDb,
    EqMid1FreqHz,
    EqMid1GainDb,
    EqMid1Q,
    EqMid2FreqHz,
    EqMid2GainDb,
    EqMid2Q,
    EqHighFreqHz,
    EqHighGainDb,

    DelayOn,
    DelayTimeMs,
    DelaySync,
    DelayFeedback,
    DelayMix,
    DelayMode,
    DelaySpread,
    DelayLowCutHz,
    DelayHighCutHz,
    DelayModRateHz,
    DelayModDepth,

    ReverbOn,
    ReverbMix,
    ReverbSize,
    ReverbDecaySec,
    ReverbDamping,
    ReverbPredelayMs,
    ReverbWidth,

    MatchEqAmount,

    Count
};

// The three oscillators have identical parameter blocks.
inline constexpr int kNumOscs = 3;
inline constexpr int kOscStride = Osc2Wave - Osc1Wave;
static_assert(Osc3Wave - Osc2Wave == kOscStride);

enum OscField : int { OscWave = 0, OscLevel, OscCoarse, OscFine, OscPulseWidth };

constexpr int osc(int oscIndex, OscField field) { return Osc1Wave + oscIndex * kOscStride + field; }

// The two LFOs have identical parameter blocks too.
inline constexpr int kNumLfos = 2;
inline constexpr int kLfoStride = Lfo2Wave - Lfo1Wave;

enum LfoField : int { LfoWave = 0, LfoRateHz, LfoTarget, LfoDepth };

constexpr int lfo(int lfoIndex, LfoField field) { return Lfo1Wave + lfoIndex * kLfoStride + field; }

} // namespace param

inline constexpr int kNumParams = param::Count;

// The match EQ curve: one gain in dB for each of its third-octave bands (see
// dsp/MatchEq.h). It is preset data rather than a set of parameters.
inline constexpr int kMatchEqBands = 31;
using MatchEqGains = std::array<float, kMatchEqBands>;

// The most copies of an oscillator that unison can stack.
inline constexpr int kMaxUnison = 7;

// Delay times for each delay_sync choice, in quarter-note beats (0 = sync off).
inline constexpr std::array<double, 8> kDelaySyncBeats{0.0, 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0};

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

// Parameters are grouped into nodes ("osc1", "filter", "delay", ...) by the
// start of their id. The node view and the preset file use these groups.
// Parameters with no node prefix ("gain_db") belong to "master".
std::span<const std::string_view> nodeNames();
std::string_view nodeOf(int index);  // "filter" for filter_cutoff_hz
std::string_view fieldOf(int index); // "cutoff_hz" for filter_cutoff_hz
std::optional<int> findParam(std::string_view node, std::string_view field);

} // namespace synthmatch
