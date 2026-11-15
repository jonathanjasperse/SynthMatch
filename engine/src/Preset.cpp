#include "synthmatch/Preset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace synthmatch {

namespace {

constexpr std::string_view kFormat = "synthmatch-preset";
constexpr std::string_view kMatchEqNode = "matcheq";
constexpr std::string_view kMatchEqGainsField = "gains_db";

void warn(std::vector<std::string>* warnings, std::string message)
{
    if (warnings)
        warnings->push_back(std::move(message));
}

void applyParam(int index, const Json& value, std::string_view label, Preset& preset,
                std::vector<std::string>* warnings)
{
    const auto& s = spec(index);
    const auto parsed = parseParamValue(s, value);
    if (!parsed) {
        warn(warnings, "ignored " + std::string(label) + ": not a valid value");
        return;
    }
    const float clamped = clampToRange(s, *parsed);
    if (std::abs(clamped - *parsed) > 1.0e-4f * std::max(1.0f, std::abs(*parsed)) && s.curve != Curve::Stepped)
        warn(warnings, std::string(label) + " was outside " + formatNumber(s.min) + ".." + formatNumber(s.max) +
                           " and has been limited");
    preset.params[static_cast<std::size_t>(index)] = clamped;
}

void applyMatchEqGains(const Json& value, Preset& preset, std::vector<std::string>* warnings)
{
    if (!value.isArray()) {
        warn(warnings, "ignored matcheq.gains_db: expected a list of numbers");
        return;
    }
    preset.matchEq.fill(0.0f);
    const auto& items = value.items();
    if (items.size() != preset.matchEq.size())
        warn(warnings, "matcheq.gains_db should have " + std::to_string(preset.matchEq.size()) + " values");
    for (std::size_t i = 0; i < std::min(items.size(), preset.matchEq.size()); ++i)
        if (items[i].isNumber())
            preset.matchEq[i] = static_cast<float>(std::clamp(items[i].number(), -24.0, 24.0));
}

} // namespace

std::optional<float> parseParamValue(const ParamSpec& s, const Json& value)
{
    if (value.isNumber())
        return static_cast<float>(value.number());

    if (value.isBool()) {
        if (s.curve == Curve::Stepped && s.min == 0.0f && s.max == 1.0f)
            return value.boolean() ? 1.0f : 0.0f;
        return std::nullopt;
    }

    if (value.isString()) {
        std::string text = value.string();
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (const auto choice = parseChoice(s, text))
            return *choice;
        // Choice names are matched in lower case, except ones like "1/8T".
        if (const auto choice = parseChoice(s, value.string()))
            return *choice;
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && *end == '\0' && std::isfinite(number))
            return static_cast<float>(number);
    }
    return std::nullopt;
}

Json paramValueToJson(const ParamSpec& s, float value)
{
    if (const auto name = choiceName(s, value); !name.empty())
        return Json(name);
    if (s.curve == Curve::Stepped)
        return Json(static_cast<int>(std::lround(value)));
    // Round to 6 significant digits so 0.3f is written as 0.3, not 0.30000001.
    return Json(std::strtod(formatNumber(value).c_str(), nullptr));
}

Json presetToJsonValue(const Preset& preset)
{
    Json nodes = Json::object();
    for (const auto node : nodeNames()) {
        Json fields = Json::object();
        for (int i = 0; i < kNumParams; ++i)
            if (nodeOf(i) == node)
                fields.set(std::string(fieldOf(i)), paramValueToJson(spec(i), preset.params[static_cast<std::size_t>(i)]));

        if (node == kMatchEqNode) {
            Json gains = Json::array();
            for (const float g : preset.matchEq)
                gains.push(Json(std::round(static_cast<double>(g) * 100.0) / 100.0));
            fields.set(std::string(kMatchEqGainsField), std::move(gains));
        }
        nodes.set(std::string(node), std::move(fields));
    }

    Json root = Json::object();
    root.set("format", Json(kFormat));
    root.set("version", Json(1));
    root.set("name", Json(preset.name));
    root.set("nodes", std::move(nodes));
    return root;
}

std::string presetToJson(const Preset& preset) { return presetToJsonValue(preset).dump(2) + "\n"; }

void applyPresetJson(const Json& root, Preset& preset, std::vector<std::string>* warnings)
{
    if (const Json* name = root.find("name"); name && name->isString())
        preset.name = name->string();

    if (const Json* nodes = root.find("nodes"); nodes && nodes->isObject()) {
        for (const auto& [node, fields] : nodes->members()) {
            if (!fields.isObject()) {
                warn(warnings, "ignored node " + node + ": expected an object");
                continue;
            }
            for (const auto& [field, value] : fields.members()) {
                if (node == kMatchEqNode && field == kMatchEqGainsField) {
                    applyMatchEqGains(value, preset, warnings);
                    continue;
                }
                const auto label = node + "." + field;
                if (const auto index = findParam(node, field))
                    applyParam(*index, value, label, preset, warnings);
                else
                    warn(warnings, "ignored " + label + ": no such parameter");
            }
        }
    }

    // The flat form, keyed by parameter id: { "params": { "filter_cutoff_hz": 900 } }.
    if (const Json* params = root.find("params"); params && params->isObject()) {
        for (const auto& [id, value] : params->members()) {
            if (const auto index = findParam(id))
                applyParam(*index, value, id, preset, warnings);
            else
                warn(warnings, "ignored " + id + ": no such parameter");
        }
    }
}

std::optional<Preset> presetFromJson(std::string_view text, std::string* error, std::vector<std::string>* warnings,
                                     const Preset& base)
{
    const auto root = Json::parse(text, error);
    if (!root)
        return std::nullopt;
    if (!root->isObject()) {
        if (error)
            *error = "a preset must be a JSON object";
        return std::nullopt;
    }
    Preset preset = base;
    applyPresetJson(*root, preset, warnings);
    return preset;
}

bool savePreset(const std::filesystem::path& path, const Preset& preset, std::string* error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file)
        file << presetToJson(preset);
    if (!file) {
        if (error)
            *error = "cannot write " + path.string();
        return false;
    }
    return true;
}

std::optional<Preset> loadPreset(const std::filesystem::path& path, std::string* error,
                                 std::vector<std::string>* warnings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error)
            *error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return presetFromJson(text.str(), error, warnings);
}

} // namespace synthmatch
