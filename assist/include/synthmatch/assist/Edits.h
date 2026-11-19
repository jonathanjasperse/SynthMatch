#pragma once

// Edits: the small vocabulary the assistant uses to change a preset.
//
// A language model's answer, or JSON pasted into the terminal, becomes the
// same thing: a short list of edits, each of which moves one parameter. That
// keeps the model's job small (pick a parameter and a direction) and means
// every change can be checked, described and undone.
//
// As JSON, which is what a language model is asked to write:
//
//   { "edits": [ { "param": "filter_cutoff_hz", "op": "nudge", "value": 0.15 },
//                { "param": "reverb_on",        "op": "set",   "value": "on" } ],
//     "reply": "Brighter, with reverb." }

#include "synthmatch/Json.h"
#include "synthmatch/Params.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace synthmatch::assist {

struct Edit {
    enum class Op {
        Set,   // value is the new value, in the parameter's own units
        Add,   // value is added to the current value
        Scale, // the current value is multiplied by value
        Nudge  // value is a fraction of the knob's travel: 0.1 = a tenth of the way up
    };

    int param = 0;
    Op op = Op::Set;
    float value = 0.0f;
};

struct Change {
    int param = 0;
    float before = 0.0f;
    float after = 0.0f;
};

// Applies the edits in order and returns what actually changed. Values are
// kept inside each parameter's range; edits that change nothing are left out.
std::vector<Change> applyEdits(ParamValues& params, std::span<const Edit> edits);

// "2.1 kHz", "350 ms", "square", "on", "40%", "-6 dB".
std::string formatValue(int param, float value);

// "Filter Cutoff 900 Hz -> 2.1 kHz".
std::string describeChange(const Change& change);
std::string describeChanges(std::span<const Change> changes);

// Edits to and from the JSON form shown above. Reading is forgiving: unknown
// parameters and malformed entries are skipped and reported in `problems`.
Json editsToJson(std::span<const Edit> edits, std::string_view reply = {});
std::vector<Edit> editsFromJson(const Json& root, std::string* reply = nullptr,
                                std::vector<std::string>* problems = nullptr);

} // namespace synthmatch::assist
