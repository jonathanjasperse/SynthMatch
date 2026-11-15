#pragma once

// A preset: every parameter value plus the match EQ curve.
//
// On disk a preset is a small JSON file, grouped by node so that a person (or
// the assistant) can read and edit it:
//
//   {
//     "format": "synthmatch-preset",
//     "version": 1,
//     "name": "Warm pad",
//     "nodes": {
//       "osc1":   { "wave": "saw", "level": 1, "coarse": 0, "fine": 0, "pw": 0.5 },
//       "filter": { "type": "lowpass", "cutoff_hz": 1800, "resonance": 0.2, ... },
//       "reverb": { "on": "on", "mix": 0.3, ... },
//       "matcheq": { "amount": 1, "gains_db": [0, 0.5, ...] }
//     }
//   }
//
// Node and field names come from the parameter ids ("filter_cutoff_hz" is
// field "cutoff_hz" of node "filter"). Anything missing keeps its default.

#include "synthmatch/Json.h"
#include "synthmatch/Params.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synthmatch {

struct Preset {
    std::string name = "Init";
    ParamValues params = defaultValues();
    MatchEqGains matchEq{}; // dB per band; all zero = no correction
};

Json presetToJsonValue(const Preset& preset);
std::string presetToJson(const Preset& preset);

// Reads a preset. Unknown nodes or fields and out-of-range values are reported
// in `warnings` and otherwise ignored or clamped; only text that isn't JSON at
// all is an error. `base` supplies the values for anything the text leaves out.
std::optional<Preset> presetFromJson(std::string_view text, std::string* error = nullptr,
                                     std::vector<std::string>* warnings = nullptr, const Preset& base = {});

// Applies the "nodes" / "params" part of a JSON object to a preset.
void applyPresetJson(const Json& root, Preset& preset, std::vector<std::string>* warnings = nullptr);

// Reads one value the way presets write them: a number, a choice name
// ("saw", "on"), a number in a string, or true/false for on/off parameters.
std::optional<float> parseParamValue(const ParamSpec& spec, const Json& value);

// The value as it appears in a preset file: a choice name or a number.
Json paramValueToJson(const ParamSpec& spec, float value);

bool savePreset(const std::filesystem::path& path, const Preset& preset, std::string* error = nullptr);
std::optional<Preset> loadPreset(const std::filesystem::path& path, std::string* error = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

} // namespace synthmatch
