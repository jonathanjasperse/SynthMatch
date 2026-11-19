// Tests for the assistant: the edit format, and the language-model prompt and
// answer handling. Plain asserts; run through CTest.

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Assistant.h"
#include "synthmatch/assist/Edits.h"
#include "synthmatch/dsp/Lfo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                                               \
        }                                                                                               \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                           \
    do {                                                                                                \
        const double a_ = (a), b_ = (b);                                                                \
        if (!(std::abs(a_ - b_) <= (tol))) {                                                            \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, expected %g +/- %g\n", __FILE__,     \
                         __LINE__, #a, a_, b_, static_cast<double>(tol));                               \
            ++g_failures;                                                                               \
        }                                                                                               \
    } while (0)

using namespace synthmatch;
using namespace synthmatch::assist;

// A patch in the middle of things, so changes in either direction are possible.
ParamValues midPatch()
{
    auto p = defaultValues();
    // Set here rather than taken from the defaults, which may change.
    p[param::GainDb] = -12.0f;
    p[param::ReverbDecaySec] = 2.0f;
    p[param::FilterCutoffHz] = 1500.0f;
    p[param::FilterResonance] = 0.3f;
    p[param::ReverbOn] = 1.0f;
    p[param::ReverbMix] = 0.3f;
    p[param::ReverbDecaySec] = 2.0f;
    p[param::AmpAttackMs] = 30.0f;
    p[param::AmpReleaseMs] = 400.0f;
    return p;
}

float v(const ParamValues& p, int index) { return p[static_cast<std::size_t>(index)]; }

void testEdits()
{
    auto p = midPatch();
    const Edit edits[] = {
        {param::FilterCutoffHz, Edit::Op::Set, 900.0f},
        {param::GainDb, Edit::Op::Add, -3.0f},
        {param::ReverbDecaySec, Edit::Op::Scale, 2.0f},
        {param::ReverbMix, Edit::Op::Nudge, 0.1f},
        {param::UnisonVoices, Edit::Op::Nudge, 0.01f}, // a stepped parameter moves at least one step
        {param::Osc1Level, Edit::Op::Add, 5.0f},       // limited to the range (and already at the top)
    };
    const auto changes = applyEdits(p, edits);
    CHECK(v(p, param::FilterCutoffHz) == 900.0f);
    CHECK(v(p, param::GainDb) == -15.0f);
    CHECK(v(p, param::ReverbDecaySec) == 4.0f);
    CHECK_NEAR(v(p, param::ReverbMix), 0.4, 1e-6);
    CHECK(v(p, param::UnisonVoices) == 2.0f);
    CHECK(v(p, param::Osc1Level) == 1.0f);
    CHECK(changes.size() == 5); // osc1 level didn't change

    // A nudge on a log parameter moves along the knob, not in Hz.
    p = midPatch();
    const Edit up[] = {{param::FilterCutoffHz, Edit::Op::Nudge, 0.1f}};
    applyEdits(p, up);
    const float expected = fromNormalized(spec(param::FilterCutoffHz), toNormalized(spec(param::FilterCutoffHz), 1500.0f) + 0.1f);
    CHECK_NEAR(v(p, param::FilterCutoffHz), expected, 0.5);

    // Descriptions read naturally.
    CHECK(formatValue(param::FilterCutoffHz, 2100.0f) == "2.1 kHz");
    CHECK(formatValue(param::FilterCutoffHz, 440.0f) == "440 Hz");
    CHECK(formatValue(param::AmpReleaseMs, 1500.0f) == "1.5 s");
    CHECK(formatValue(param::AmpAttackMs, 20.0f) == "20 ms");
    CHECK(formatValue(param::ReverbMix, 0.4f) == "40%");
    CHECK(formatValue(param::ReverbBassMult, 1.5f) == "1.5x");
    CHECK(formatValue(param::Osc1Wave, 3.0f) == "square");
    CHECK(formatValue(param::GainDb, -6.0f) == "-6 dB");
    CHECK(describeChange({param::FilterCutoffHz, 900.0f, 2100.0f}) == "Filter Cutoff 900 Hz -> 2.1 kHz");

    // To JSON and back.
    const Edit some[] = {{param::FilterCutoffHz, Edit::Op::Nudge, 0.15f}, {param::ReverbOn, Edit::Op::Set, 1.0f}};
    const Json json = editsToJson(some, "Brighter, with reverb.");
    std::string reply;
    const auto back = editsFromJson(json, &reply);
    CHECK(reply == "Brighter, with reverb.");
    CHECK(back.size() == 2);
    if (back.size() == 2) {
        CHECK(back[0].param == param::FilterCutoffHz && back[0].op == Edit::Op::Nudge && back[0].value == 0.15f);
        CHECK(back[1].param == param::ReverbOn && back[1].op == Edit::Op::Set && back[1].value == 1.0f);
    }
    CHECK(json.dump(0).find("\"value\":\"on\"") != std::string::npos);
}

void testDescribe()
{
    Preset preset;
    preset.name = "Test";
    preset.params = midPatch();
    preset.params[param::Osc2Level] = 0.5f;
    preset.params[param::Osc2Coarse] = -12.0f;
    const std::string text = describePreset(preset);
    CHECK(text.find("saw") != std::string::npos);
    CHECK(text.find("1.5 kHz") != std::string::npos);
    CHECK(text.find("reverb hall") != std::string::npos);
    CHECK(text.find("-12 st") != std::string::npos);
}

void testModelPrompt()
{
    const auto p = midPatch();
    const std::string system = modelSystemPrompt();
    const std::string user = modelUserPrompt("make it warmer", p);
    CHECK(system.find("filter_cutoff_hz") != std::string::npos);
    CHECK(user.find("filter_cutoff_hz = 1500") != std::string::npos);
    CHECK(user.find("reverb_on = on") != std::string::npos);
    CHECK(user.find("Request: make it warmer") != std::string::npos);
    // The model can reach the reverb's mode and tone, and is told that a
    // darker reverb isn't a darker filter.
    CHECK(user.find("reverb_mode = hall") != std::string::npos);
    CHECK(user.find("reverb_highcut_hz") != std::string::npos);
    CHECK(system.find("darker reverb") != std::string::npos);

    // The schema is valid JSON naming the parameters the model may use.
    const Json schema = modelReplySchema();
    const auto again = Json::parse(schema.dump(0));
    CHECK(again.has_value());
    CHECK(schema.dump(0).find("\"filter_cutoff_hz\"") != std::string::npos);

    // Every example in the system prompt is an answer we can read.
    std::size_t at = 0, examples = 0;
    while ((at = system.find("{\"edits\"", at)) != std::string::npos) {
        const std::size_t end = system.find('\n', at);
        const auto reply = parseModelReply(system.substr(at, end - at));
        CHECK(reply.has_value());
        ++examples;
        at = end;
    }
    CHECK(examples >= 4);
}

void testModelReply()
{
    std::string error;

    // Plain JSON.
    auto reply = parseModelReply(R"({"edits":[{"param":"filter_cutoff_hz","op":"nudge","value":0.1}],"reply":"Brighter."})");
    CHECK(reply && reply->edits.size() == 1 && reply->text == "Brighter.");

    // Wrapped in prose and a code fence, with a trailing comma.
    reply = parseModelReply("Sure! Here you go:\n```json\n{\"edits\": [{\"param\": \"reverb_mix\", \"op\": \"nudge\", "
                            "\"value\": 0.2},], \"reply\": \"More space.\"}\n```\nEnjoy.");
    CHECK(reply && reply->edits.size() == 1 && reply->edits[0].param == param::ReverbMix);

    // The preset file's own form.
    reply = parseModelReply(R"({"nodes": {"filter": {"cutoff_hz": 900, "type": "bandpass"}, "reverb": {"on": "on"}}})");
    CHECK(reply && reply->edits.size() == 3);

    // Asked for vibrato, the model's answer always ends up on pitch.
    {
        auto wrong = parseModelReply(R"({"edits":[{"param":"lfo1_target","op":"set","value":"amp"},
                                                  {"param":"lfo1_wave","op":"set","value":"square"}]})");
        CHECK(wrong.has_value());
        if (wrong) {
            reinforceModelReply("give it a dreamy vibrato please", *wrong);
            auto p = defaultValues();
            applyEdits(p, wrong->edits);
            CHECK(v(p, param::Lfo1Target) == static_cast<float>(dsp::LfoTarget::Pitch));
            CHECK(v(p, param::Lfo1Wave) == static_cast<float>(dsp::LfoWave::Sine));
            CHECK_NEAR(v(p, param::Lfo1RateHz), 5.5, 1e-6);
            CHECK(v(p, param::Lfo1Depth) > 0.05f);
        }
        auto off = parseModelReply(R"({"edits":[{"param":"lfo1_target","op":"set","value":"off"}]})");
        if (off) {
            reinforceModelReply("no vibrato on this one", *off);
            CHECK(off->edits.size() == 1);
        }
    }

    // Unknown parameters are skipped. A nudge above 1 is an amount in the
    // parameter's units; smaller nudges are kept to a sensible step.
    reply = parseModelReply(R"({"edits":[{"param":"sparkle","op":"set","value":1},
                                         {"param":"eq_low_gain_db","op":"nudge","value":2},
                                         {"param":"filter_resonance","op":"nudge","value":0.9}]})");
    CHECK(reply && reply->edits.size() == 2);
    if (reply && reply->edits.size() == 2) {
        CHECK(reply->edits[0].op == Edit::Op::Add && reply->edits[0].value == 2.0f);
        CHECK(reply->edits[1].op == Edit::Op::Nudge && reply->edits[1].value == 0.35f);
    }

    // Nothing usable.
    CHECK(!parseModelReply("I think it sounds great already!", &error));
    CHECK(!error.empty());
    CHECK(!parseModelReply(R"({"edits": []})", &error));
    CHECK(!parseModelReply(R"({"edits": [{"param": "banana"}]})", &error));
}

} // namespace

int main()
{
    testEdits();
    testDescribe();
    testModelPrompt();
    testModelReply();

    if (g_failures == 0)
        std::puts("All assistant tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
