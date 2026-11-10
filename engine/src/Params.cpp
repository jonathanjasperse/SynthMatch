#include "synthmatch/Params.h"

#include "synthmatch/dsp/Oscillator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace synthmatch {

namespace {

// Same order as dsp::Wave.
constexpr std::array<std::string_view, 4> kWaveNames{"sine", "triangle", "saw", "square"};
static_assert(kWaveNames.size() == static_cast<std::size_t>(dsp::Wave::Count));

constexpr float kSaw = static_cast<float>(dsp::Wave::Saw);
constexpr float kLastWave = static_cast<float>(dsp::Wave::Count) - 1.0f;

// Same order as param::Index. Never reorder or reuse ids: saved presets and
// trained models refer to parameters by id.
constexpr std::array<ParamSpec, kNumParams> kSchema{{
    {"gain_db", "Gain", -60.0f, 0.0f, -12.0f, Curve::Linear, "dB"},
    {"amp_attack_ms", "Amp Attack", 0.1f, 5000.0f, 5.0f, Curve::Log, "ms"},
    {"amp_release_ms", "Amp Release", 1.0f, 10000.0f, 200.0f, Curve::Log, "ms"},
    {"amp_decay_ms", "Amp Decay", 1.0f, 10000.0f, 200.0f, Curve::Log, "ms"},
    {"amp_sustain", "Amp Sustain", 0.0f, 1.0f, 1.0f, Curve::Linear, ""},

    {"osc1_wave", "Osc 1 Wave", 0.0f, kLastWave, kSaw, Curve::Stepped, "", kWaveNames},
    {"osc1_level", "Osc 1 Level", 0.0f, 1.0f, 1.0f, Curve::Linear, ""},
    {"osc1_coarse", "Osc 1 Coarse", -36.0f, 36.0f, 0.0f, Curve::Stepped, "st"},
    {"osc1_fine", "Osc 1 Fine", -100.0f, 100.0f, 0.0f, Curve::Linear, "cents"},
    {"osc1_pw", "Osc 1 Pulse Width", 0.05f, 0.95f, 0.5f, Curve::Linear, ""},

    {"osc2_wave", "Osc 2 Wave", 0.0f, kLastWave, kSaw, Curve::Stepped, "", kWaveNames},
    {"osc2_level", "Osc 2 Level", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
    {"osc2_coarse", "Osc 2 Coarse", -36.0f, 36.0f, 0.0f, Curve::Stepped, "st"},
    {"osc2_fine", "Osc 2 Fine", -100.0f, 100.0f, 0.0f, Curve::Linear, "cents"},
    {"osc2_pw", "Osc 2 Pulse Width", 0.05f, 0.95f, 0.5f, Curve::Linear, ""},

    {"osc3_wave", "Osc 3 Wave", 0.0f, kLastWave, kSaw, Curve::Stepped, "", kWaveNames},
    {"osc3_level", "Osc 3 Level", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
    {"osc3_coarse", "Osc 3 Coarse", -36.0f, 36.0f, 0.0f, Curve::Stepped, "st"},
    {"osc3_fine", "Osc 3 Fine", -100.0f, 100.0f, 0.0f, Curve::Linear, "cents"},
    {"osc3_pw", "Osc 3 Pulse Width", 0.05f, 0.95f, 0.5f, Curve::Linear, ""},

    {"noise_level", "Noise Level", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
}};

} // namespace

std::span<const ParamSpec> schema() { return kSchema; }

const ParamSpec& spec(int index) { return kSchema[static_cast<std::size_t>(index)]; }

std::optional<int> findParam(std::string_view id)
{
    for (std::size_t i = 0; i < kSchema.size(); ++i)
        if (kSchema[i].id == id)
            return static_cast<int>(i);
    return std::nullopt;
}

float clampToRange(const ParamSpec& s, float value)
{
    value = std::clamp(value, s.min, s.max);
    if (s.curve == Curve::Stepped)
        value = std::round(value);
    return value;
}

float toNormalized(const ParamSpec& s, float value)
{
    value = clampToRange(s, value);
    if (s.curve == Curve::Log)
        return std::log(value / s.min) / std::log(s.max / s.min);
    return (value - s.min) / (s.max - s.min);
}

float fromNormalized(const ParamSpec& s, float normalized)
{
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    if (s.curve == Curve::Log)
        return clampToRange(s, s.min * std::pow(s.max / s.min, normalized));
    return clampToRange(s, s.min + normalized * (s.max - s.min));
}

std::string_view choiceName(const ParamSpec& s, float value)
{
    if (s.choices.empty())
        return {};
    const auto index = static_cast<std::size_t>(clampToRange(s, value) - s.min);
    return index < s.choices.size() ? s.choices[index] : std::string_view{};
}

std::optional<float> parseChoice(const ParamSpec& s, std::string_view name)
{
    for (std::size_t i = 0; i < s.choices.size(); ++i)
        if (s.choices[i] == name)
            return s.min + static_cast<float>(i);
    return std::nullopt;
}

ParamValues defaultValues()
{
    ParamValues values{};
    for (std::size_t i = 0; i < kSchema.size(); ++i)
        values[i] = kSchema[i].defaultValue;
    return values;
}

} // namespace synthmatch
