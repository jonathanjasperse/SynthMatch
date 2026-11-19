#include "synthmatch/assist/Assistant.h"

#include "synthmatch/dsp/Lfo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace synthmatch::assist {

namespace {

// Lower-case, with punctuation turned into spaces and "|" between clauses, for
// finding whole words in a request.
std::string normalise(std::string_view text)
{
    std::string out;
    const auto isDigit = [&](std::size_t i) { return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool atWordStart = out.empty() || out.back() == ' ';
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (c == '.') {
            if (isDigit(i + 1) && (atWordStart || std::isdigit(static_cast<unsigned char>(out.back()))))
                out += '.';
            else
                out += " | ";
        } else if (c == ',' || c == ';' || c == '!' || c == '?' || c == '\n') {
            out += " | ";
        } else if ((c == '-' || c == '+') && atWordStart && (isDigit(i + 1) || (text.size() > i + 1 && text[i + 1] == '.'))) {
            out += static_cast<char>(c);
        } else if (c == '%' || c == '/' || c == '_') {
            out += static_cast<char>(c);
        } else if (c == '\'') {
            // "it's" -> "its"
        } else if (!atWordStart) {
            out += ' ';
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// Position of a phrase as whole words, or npos.
std::size_t findPhrase(const std::string& clause, std::string_view phrase)
{
    const std::string padded = " " + clause + " ";
    const std::string needle = " " + std::string(phrase) + " ";
    const std::size_t at = padded.find(needle);
    return at; // index into `padded` is index into `clause`
}

bool has(const std::string& clause, std::string_view phrase) { return findPhrase(clause, phrase) != std::string::npos; }

bool hasAny(const std::string& clause, std::initializer_list<std::string_view> phrases)
{
    for (const auto p : phrases)
        if (has(clause, p))
            return true;
    return false;
}

// The parameters a language model is shown. A small model does better with a
// short list of the controls that matter than with all of them.
constexpr int kModelParams[] = {
    param::FilterCutoffHz, param::FilterResonance, param::FilterType,   param::FilterEnvAmount, param::FilterDecayMs,
    param::AmpAttackMs,    param::AmpDecayMs,      param::AmpSustain,   param::AmpReleaseMs,    param::GainDb,
    param::Osc1Wave,       param::Osc2Wave,        param::Osc2Level,    param::Osc2Coarse,      param::Osc2Fine,
    param::Osc3Wave,       param::Osc3Level,       param::Osc3Coarse,   param::NoiseLevel,      param::UnisonVoices,
    param::UnisonDetune,   param::UnisonSpread,    param::Lfo1Target,   param::Lfo1RateHz,      param::Lfo1Depth,
    param::EqOn,           param::EqLowGainDb,     param::EqHighGainDb, param::DelayOn,         param::DelayTimeMs,
    param::DelayFeedback,  param::DelayMix,        param::ReverbOn,     param::ReverbMix,       param::ReverbSize,
    param::ReverbDecaySec, param::MatchEqAmount,   param::ReverbMode,   param::ReverbDamping,   param::ReverbHighCutHz,
    param::ReverbModDepth, param::ReverbLowCutHz,
};

} // namespace

std::string describePreset(const Preset& preset)
{
    const auto& p = preset.params;
    const auto show = [&](int index) { return formatValue(index, p[static_cast<std::size_t>(index)]); };
    std::string out = "\"" + preset.name + "\"\n";

    out += "Oscillators: ";
    bool first = true;
    for (int k = 0; k < param::kNumOscs; ++k) {
        if (p[static_cast<std::size_t>(param::osc(k, param::OscLevel))] <= 0.0f)
            continue;
        if (!first)
            out += " + ";
        first = false;
        out += show(param::osc(k, param::OscWave));
        const float coarse = p[static_cast<std::size_t>(param::osc(k, param::OscCoarse))];
        if (coarse != 0.0f)
            out += (coarse > 0.0f ? " +" : " ") + show(param::osc(k, param::OscCoarse));
        out += " (" + show(param::osc(k, param::OscLevel)) + ")";
    }
    if (first)
        out += "none";
    if (p[param::NoiseLevel] > 0.0f)
        out += ", noise " + show(param::NoiseLevel);
    if (p[param::UnisonVoices] > 1.0f)
        out += "; unison " + show(param::UnisonVoices) + " voices, " + show(param::UnisonDetune);
    out += "\n";

    out += "Filter: " + show(param::FilterType) + " at " + show(param::FilterCutoffHz) + ", resonance " +
           show(param::FilterResonance);
    if (p[param::FilterEnvAmount] != 0.0f)
        out += ", envelope " + show(param::FilterEnvAmount);
    out += "\n";

    out += "Amp: attack " + show(param::AmpAttackMs) + ", decay " + show(param::AmpDecayMs) + ", sustain " +
           show(param::AmpSustain) + ", release " + show(param::AmpReleaseMs) + ", gain " + show(param::GainDb) + "\n";

    for (int j = 0; j < param::kNumLfos; ++j) {
        if (p[static_cast<std::size_t>(param::lfo(j, param::LfoTarget))] == 0.0f)
            continue;
        out += "LFO " + std::to_string(j + 1) + ": " + show(param::lfo(j, param::LfoWave)) + " at " +
               show(param::lfo(j, param::LfoRateHz)) + " on " + show(param::lfo(j, param::LfoTarget)) + ", depth " +
               show(param::lfo(j, param::LfoDepth)) + "\n";
    }

    std::string effects;
    const auto effect = [&](const std::string& text) { effects += (effects.empty() ? "" : "; ") + text; };
    if (p[param::EqOn] >= 0.5f)
        effect("EQ low " + show(param::EqLowGainDb) + ", high " + show(param::EqHighGainDb));
    if (p[param::DelayOn] >= 0.5f)
        effect("delay " + (p[param::DelaySync] > 0.0f ? show(param::DelaySync) : show(param::DelayTimeMs)) + ", mix " +
               show(param::DelayMix));
    if (p[param::ReverbOn] >= 0.5f)
        effect("reverb " + show(param::ReverbMode) + " " + show(param::ReverbDecaySec) + ", mix " + show(param::ReverbMix));
    bool matchEq = false;
    for (const float g : preset.matchEq)
        matchEq = matchEq || g != 0.0f;
    if (matchEq)
        effect("match EQ at " + show(param::MatchEqAmount));
    out += "Effects: " + (effects.empty() ? std::string("none") : effects);
    return out;
}

// ---------------------------------------------------------------- language model

std::string modelSystemPrompt()
{
    // Small models follow a lookup table far better than they reason about
    // sound, so the prompt is mostly a glossary of sound words.
    return "You control a synthesizer. The user asks for a change in its sound. You answer with edits to its "
           "parameters, as JSON and nothing else.\n"
           "\n"
           "Each edit has a \"param\", an \"op\" and a \"value\":\n"
           "- op \"nudge\": value is a fraction of the knob's travel. 0.1 is a small step up, 0.25 a large one. "
           "Negative values move down.\n"
           "- op \"set\": value is the exact new value: a number, or a name such as \"on\", \"off\", \"square\" or \"pitch\".\n"
           "Change only what the request asks for. A request with two parts (\"brighter and wider\") needs edits for "
           "both parts. Put one short sentence saying what you did in \"reply\".\n"
           "\n"
           "Sound words and what to change:\n"
           "brighter, more open, crisper, more bite: filter_cutoff_hz up.\n"
           "darker, duller, muffled, mellow: filter_cutoff_hz down.\n"
           "warmer, analog: filter_cutoff_hz down a little, and eq_low_gain_db up 2 (with eq_on on).\n"
           "harsh, aggressive, edgy: filter_resonance up. Less harsh, smoother: filter_resonance down and filter_cutoff_hz down a little.\n"
           "squelchy, acid, resonant: filter_resonance set 0.75, filter_env_amount set 4, filter_decay_ms set 220, filter_sustain set 0.1.\n"
           "wider, fatter, thicker, lush, supersaw: unison_voices set 5, unison_detune up. Thinner, narrower: unison_voices set 1.\n"
           "unison_detune: 8 to 15 cents is subtle, 20 to 30 a supersaw.\n"
           "punchy, snappy, tight: amp_attack_ms set 1, filter_env_amount set 2, filter_decay_ms set 160.\n"
           "soft, swelling, fades in: amp_attack_ms up.\n"
           "pad: amp_attack_ms set 350, amp_release_ms set 1800, amp_sustain set 0.85.\n"
           "pluck, plucky, plucked: amp_sustain set 0, amp_decay_ms set 350, filter_env_amount set 3.\n"
           "longer, rings out: amp_release_ms up. Shorter, staccato: amp_release_ms down.\n"
           "vibrato: lfo1_target set pitch, lfo1_rate_hz set 5.5, lfo1_depth set 0.12.\n"
           "wobble, movement: lfo1_target set cutoff, lfo1_rate_hz set 2, lfo1_depth set 0.4. Tremolo: lfo1_target set amp.\n"
           "sub, more bass weight: osc3_wave set sine, osc3_coarse set -12, osc3_level set 0.7.\n"
           "reverb, space, wet, dreamy, distant: reverb_on set on, reverb_mix up by a clear step (0.15: people want to "
           "hear it), and reverb_decay_s up so the tail is longer. Big hall: also reverb_size up, reverb_decay_s up more.\n"
           "plate, hall, room, chamber, vintage: reverb_mode set that name (and reverb_on set on).\n"
           "darker reverb: reverb_highcut_hz down 0.15 and reverb_damping up 0.15. Never filter_cutoff_hz.\n"
           "lush, chorused, swirly reverb: reverb_mod_depth up 0.2.\n"
           "less muddy reverb: reverb_lowcut_hz set 200.\n"
           "shimmer: not available. Don't touch the EQ for it.\n"
           "dry, closer: reverb_mix down, delay_mix down.\n"
           "echo, delay: delay_on set on, delay_mix up.\n"
           "breathy, noisy, airy: noise_level up. Cleaner: noise_level down.\n"
           "bass, low end: eq_low_gain_db up (eq_on on). Treble, air, sparkle: eq_high_gain_db up (eq_on on).\n"
           "louder: gain_db up 3. Quieter: gain_db down 3.\n"
           "octave up or down: osc1_coarse, osc2_coarse and osc3_coarse up or down 12.\n"
           "A little, a bit, slightly: smaller steps (about 0.05). Much, a lot, very: larger steps (about 0.25).\n"
           "\n"
           "Examples:\n"
           "Request: make it brighter\n"
           "{\"edits\":[{\"param\":\"filter_cutoff_hz\",\"op\":\"nudge\",\"value\":0.15}],\"reply\":\"Opened the filter.\"}\n"
           "Request: add more reverb\n"
           "{\"edits\":[{\"param\":\"reverb_on\",\"op\":\"set\",\"value\":\"on\"},{\"param\":\"reverb_mix\",\"op\":\"nudge\",\"value\":0.15},"
           "{\"param\":\"reverb_decay_s\",\"op\":\"nudge\",\"value\":0.08}],\"reply\":\"More reverb, with a longer tail.\"}\n"
           "Request: darker plate reverb\n"
           "{\"edits\":[{\"param\":\"reverb_on\",\"op\":\"set\",\"value\":\"on\"},{\"param\":\"reverb_mode\",\"op\":\"set\",\"value\":\"plate\"},"
           "{\"param\":\"reverb_highcut_hz\",\"op\":\"nudge\",\"value\":-0.15},{\"param\":\"reverb_damping\",\"op\":\"nudge\",\"value\":0.15}],"
           "\"reply\":\"A darker plate.\"}\n"
           "Request: darker and a lot wider\n"
           "{\"edits\":[{\"param\":\"filter_cutoff_hz\",\"op\":\"nudge\",\"value\":-0.12},{\"param\":\"unison_voices\",\"op\":\"set\",\"value\":7},"
           "{\"param\":\"unison_detune\",\"op\":\"nudge\",\"value\":0.2}],\"reply\":\"Darker, with a wide unison stack.\"}\n"
           "Request: make it a plucked string\n"
           "{\"edits\":[{\"param\":\"amp_sustain\",\"op\":\"set\",\"value\":0},{\"param\":\"amp_decay_ms\",\"op\":\"set\",\"value\":350},"
           "{\"param\":\"filter_env_amount\",\"op\":\"set\",\"value\":3}],\"reply\":\"Short decay with a filter pluck.\"}\n"
           "Request: a slow wobble\n"
           "{\"edits\":[{\"param\":\"lfo1_target\",\"op\":\"set\",\"value\":\"cutoff\"},{\"param\":\"lfo1_rate_hz\",\"op\":\"set\",\"value\":1},"
           "{\"param\":\"lfo1_depth\",\"op\":\"set\",\"value\":0.4}],\"reply\":\"A slow filter wobble.\"}";
}

std::string modelUserPrompt(std::string_view request, const ParamValues& current)
{
    std::string out = "Current settings (with each parameter's range):\n";
    for (const int index : kModelParams) {
        const auto& s = spec(index);
        out += std::string(s.id) + " = ";
        const float value = current[static_cast<std::size_t>(index)];
        if (!s.choices.empty()) {
            out += std::string(choiceName(s, value)) + " (one of:";
            for (const auto choice : s.choices)
                out += " " + std::string(choice);
            out += ")";
        } else {
            out += formatNumber(value) + " (" + formatNumber(s.min) + " to " + formatNumber(s.max) +
                   (s.unit.empty() ? "" : " " + std::string(s.unit)) + ")";
        }
        out += "\n";
    }
    out += "\nRequest: ";
    out += request;
    return out;
}

Json modelReplySchema()
{
    Json ids = Json::array();
    for (const int index : kModelParams)
        ids.push(Json(spec(index).id));

    Json ops = Json::array();
    ops.push(Json("nudge"));
    ops.push(Json("set"));

    Json valueTypes = Json::array();
    valueTypes.push(Json("number"));
    valueTypes.push(Json("string"));

    const auto typed = [](const char* type) {
        Json j = Json::object();
        j.set("type", Json(type));
        return j;
    };

    Json paramSchema = typed("string");
    paramSchema.set("enum", std::move(ids));
    Json opSchema = typed("string");
    opSchema.set("enum", std::move(ops));
    Json valueSchema = Json::object();
    valueSchema.set("type", std::move(valueTypes));

    Json editProps = Json::object();
    editProps.set("param", std::move(paramSchema));
    editProps.set("op", std::move(opSchema));
    editProps.set("value", std::move(valueSchema));
    Json editRequired = Json::array();
    editRequired.push(Json("param"));
    editRequired.push(Json("op"));
    editRequired.push(Json("value"));
    Json edit = typed("object");
    edit.set("properties", std::move(editProps));
    edit.set("required", std::move(editRequired));

    Json editList = typed("array");
    editList.set("items", std::move(edit));

    Json props = Json::object();
    props.set("edits", std::move(editList));
    props.set("reply", typed("string"));
    Json required = Json::array();
    required.push(Json("edits"));
    required.push(Json("reply"));

    Json root = typed("object");
    root.set("properties", std::move(props));
    root.set("required", std::move(required));
    return root;
}

std::optional<ModelReply> parseModelReply(std::string_view answer, std::string* error)
{
    const auto fail = [&](const char* message) -> std::optional<ModelReply> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    // Models sometimes wrap the JSON in prose or a code fence: take the outermost braces.
    const std::size_t open = answer.find('{');
    const std::size_t close = answer.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return fail("the model's answer contained no JSON");

    std::string parseError;
    const auto root = Json::parse(answer.substr(open, close - open + 1), &parseError);
    if (!root || !root->isObject())
        return fail("the model's answer was not valid JSON");

    ModelReply reply;
    std::vector<std::string> problems;
    reply.edits = editsFromJson(*root, &reply.text, &problems);
    if (reply.edits.empty())
        return fail("the model's answer contained no usable edits");

    // Small models get the scale of a nudge wrong. A nudge above 1 is read as
    // an amount in the parameter's own units ("nudge 2" on a gain in dB means
    // +2 dB), and ordinary nudges are kept to a sensible step.
    for (auto& e : reply.edits) {
        if (e.op != Edit::Op::Nudge)
            continue;
        if (std::abs(e.value) > 1.0f)
            e.op = Edit::Op::Add;
        else
            e.value = std::clamp(e.value, -0.35f, 0.35f);
    }
    return reply;
}

void reinforceModelReply(std::string_view request, ModelReply& reply)
{
    const std::string clause = normalise(request);
    if (!has(clause, "vibrato") || hasAny(clause, {"no vibrato", "without vibrato", "less vibrato", "remove the vibrato",
                                                   "remove vibrato", "stop the vibrato", "turn off the vibrato"}))
        return;

    const auto pitch = static_cast<float>(dsp::LfoTarget::Pitch);
    bool target = false, rate = false, depth = false;
    for (auto& e : reply.edits) {
        if (e.param == param::Lfo1Target) {
            e.op = Edit::Op::Set;
            e.value = pitch;
            target = true;
        } else if (e.param == param::Lfo1Wave) {
            const auto wave = static_cast<dsp::LfoWave>(static_cast<int>(e.value));
            if (e.op == Edit::Op::Set && wave != dsp::LfoWave::Sine && wave != dsp::LfoWave::Triangle)
                e.value = static_cast<float>(dsp::LfoWave::Sine);
        }
        rate = rate || e.param == param::Lfo1RateHz;
        depth = depth || e.param == param::Lfo1Depth;
    }
    if (!target)
        reply.edits.push_back({param::Lfo1Target, Edit::Op::Set, pitch});
    if (!rate)
        reply.edits.push_back({param::Lfo1RateHz, Edit::Op::Set, 5.5f});
    if (!depth)
        reply.edits.push_back({param::Lfo1Depth, Edit::Op::Set, 0.12f});
}

} // namespace synthmatch::assist
