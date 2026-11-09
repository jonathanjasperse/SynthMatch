#include "synthmatch/Params.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace synthmatch {

namespace {

// Same order as param::Index. Never reorder or reuse ids: saved presets and
// trained models refer to parameters by id.
constexpr std::array<ParamSpec, kNumParams> kSchema{{
    {"gain_db", "Gain", -60.0f, 0.0f, -12.0f, Curve::Linear, "dB"},
    {"amp_attack_ms", "Amp Attack", 0.1f, 5000.0f, 5.0f, Curve::Log, "ms"},
    {"amp_release_ms", "Amp Release", 1.0f, 10000.0f, 200.0f, Curve::Log, "ms"},
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

ParamValues defaultValues()
{
    ParamValues values{};
    for (std::size_t i = 0; i < kSchema.size(); ++i)
        values[i] = kSchema[i].defaultValue;
    return values;
}

} // namespace synthmatch
