#include "synthmatch/assist/Edits.h"

#include "synthmatch/Preset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace synthmatch::assist {

namespace {

std::string number(double value, int decimals)
{
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    std::string s = buffer;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

constexpr const char* kOpNames[] = {"set", "add", "scale", "nudge"};

} // namespace

std::vector<Change> applyEdits(ParamValues& params, std::span<const Edit> edits)
{
    std::vector<Change> changes;
    for (const auto& edit : edits) {
        if (edit.param < 0 || edit.param >= kNumParams || !std::isfinite(edit.value))
            continue;
        const auto& s = spec(edit.param);
        float& slot = params[static_cast<std::size_t>(edit.param)];
        const float before = slot;
        float after = before;

        switch (edit.op) {
        case Edit::Op::Set:
            after = edit.value;
            break;
        case Edit::Op::Add:
            after = before + edit.value;
            break;
        case Edit::Op::Scale:
            after = before * edit.value;
            break;
        case Edit::Op::Nudge:
            if (s.curve == Curve::Stepped) {
                // Switches and step counts move by whole steps: at least one.
                const float steps = edit.value * (s.max - s.min);
                const float whole = std::abs(steps) < 1.0f ? (edit.value > 0.0f ? 1.0f : -1.0f) : std::round(steps);
                after = edit.value == 0.0f ? before : before + whole;
            } else {
                after = fromNormalized(s, std::clamp(toNormalized(s, before) + edit.value, 0.0f, 1.0f));
            }
            break;
        }

        after = clampToRange(s, after);
        if (after == before)
            continue;
        slot = after;

        // Several edits to one parameter are reported as one change.
        const auto existing = std::find_if(changes.begin(), changes.end(),
                                           [&](const Change& c) { return c.param == edit.param; });
        if (existing != changes.end())
            existing->after = after;
        else
            changes.push_back({edit.param, before, after});
    }

    changes.erase(std::remove_if(changes.begin(), changes.end(), [](const Change& c) { return c.before == c.after; }),
                  changes.end());
    return changes;
}

std::string formatValue(int param, float value)
{
    const auto& s = spec(param);
    if (const auto name = choiceName(s, value); !name.empty())
        return std::string(name);

    const double v = value;
    if (s.unit == "Hz")
        return v >= 1000.0 ? number(v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz" : number(v, v < 100.0 ? 1 : 0) + " Hz";
    if (s.unit == "ms")
        return v >= 1000.0 ? number(v / 1000.0, 2) + " s" : number(v, v < 10.0 ? 1 : 0) + " ms";
    if (s.unit == "s")
        return number(v, 2) + " s";
    if (s.unit == "dB")
        return (v > 0.0 ? "+" : "") + number(v, 1) + " dB";
    if (s.unit == "st")
        return number(v, 0) + " st";
    if (s.unit == "cents")
        return number(v, 0) + " cents";
    if (s.unit == "oct")
        return (v > 0.0 ? "+" : "") + number(v, 2) + " oct";
    if (s.curve == Curve::Stepped)
        return number(v, 0);
    if (s.min == 0.0f && s.max == 1.0f)
        return number(v * 100.0, 0) + "%";
    return number(v, 2);
}

std::string describeChange(const Change& change)
{
    return std::string(spec(change.param).name) + " " + formatValue(change.param, change.before) + " -> " +
           formatValue(change.param, change.after);
}

std::string describeChanges(std::span<const Change> changes)
{
    std::string out;
    for (const auto& c : changes) {
        if (!out.empty())
            out += "\n";
        out += describeChange(c);
    }
    return out;
}

Json editsToJson(std::span<const Edit> edits, std::string_view reply)
{
    Json list = Json::array();
    for (const auto& edit : edits) {
        if (edit.param < 0 || edit.param >= kNumParams)
            continue;
        const auto& s = spec(edit.param);
        Json item = Json::object();
        item.set("param", Json(s.id));
        item.set("op", Json(kOpNames[static_cast<int>(edit.op)]));
        item.set("value", edit.op == Edit::Op::Set ? paramValueToJson(s, edit.value)
                                                    : Json(std::strtod(formatNumber(edit.value).c_str(), nullptr)));
        list.push(std::move(item));
    }
    Json root = Json::object();
    root.set("edits", std::move(list));
    root.set("reply", Json(reply));
    return root;
}

std::vector<Edit> editsFromJson(const Json& root, std::string* reply, std::vector<std::string>* problems)
{
    const auto problem = [&](std::string message) {
        if (problems)
            problems->push_back(std::move(message));
    };

    std::vector<Edit> edits;
    if (reply) {
        reply->clear();
        if (const Json* r = root.find("reply"); r && r->isString())
            *reply = r->string();
    }

    // The preset file's own form also works: { "nodes": { "filter": { "cutoff_hz": 900 } } }
    // or { "params": { "filter_cutoff_hz": 900 } }. Each value becomes a "set".
    const auto addSet = [&](int index, const Json& value, const std::string& label) {
        if (const auto parsed = parseParamValue(spec(index), value))
            edits.push_back({index, Edit::Op::Set, *parsed});
        else
            problem("bad value for " + label);
    };
    if (const Json* nodes = root.find("nodes"); nodes && nodes->isObject()) {
        for (const auto& [node, fields] : nodes->members()) {
            if (!fields.isObject())
                continue;
            for (const auto& [field, value] : fields.members()) {
                if (const auto index = findParam(node, field))
                    addSet(*index, value, node + "." + field);
                else if (!(node == "matcheq" && field == "gains_db"))
                    problem("no parameter called " + node + "." + field);
            }
        }
    }
    if (const Json* params = root.find("params"); params && params->isObject()) {
        for (const auto& [id, value] : params->members()) {
            if (const auto index = findParam(id))
                addSet(*index, value, id);
            else
                problem("no parameter called " + id);
        }
    }

    const Json* list = root.find("edits");
    if (!list || !list->isArray()) {
        if (edits.empty())
            problem("no \"edits\" list");
        return edits;
    }

    for (const auto& item : list->items()) {
        const Json* id = item.find("param");
        const Json* value = item.find("value");
        if (!item.isObject() || !id || !id->isString() || !value) {
            problem("an edit is missing \"param\" or \"value\"");
            continue;
        }
        const auto index = findParam(id->string());
        if (!index) {
            problem("no parameter called " + id->string());
            continue;
        }

        Edit edit;
        edit.param = *index;
        edit.op = Edit::Op::Set;
        if (const Json* op = item.find("op"); op && op->isString()) {
            bool known = false;
            for (int k = 0; k < 4; ++k) {
                if (op->string() == kOpNames[k]) {
                    edit.op = static_cast<Edit::Op>(k);
                    known = true;
                }
            }
            if (!known) {
                problem("unknown op " + op->string());
                continue;
            }
        }

        if (edit.op == Edit::Op::Set) {
            const auto parsed = parseParamValue(spec(edit.param), *value);
            if (!parsed) {
                problem("bad value for " + id->string());
                continue;
            }
            edit.value = *parsed;
        } else {
            if (!value->isNumber()) {
                problem("bad value for " + id->string());
                continue;
            }
            edit.value = static_cast<float>(value->number());
        }
        edits.push_back(edit);
    }
    return edits;
}

} // namespace synthmatch::assist
