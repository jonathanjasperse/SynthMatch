// Tests for the assistant: the built-in rules, the edit format, and the
// language-model prompt and answer handling. Plain asserts; run through CTest.

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Assistant.h"
#include "synthmatch/assist/Edits.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"

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
    p[param::FilterCutoffHz] = 1500.0f;
    p[param::FilterResonance] = 0.3f;
    p[param::ReverbOn] = 1.0f;
    p[param::ReverbMix] = 0.3f;
    p[param::AmpAttackMs] = 30.0f;
    p[param::AmpReleaseMs] = 400.0f;
    return p;
}

// Applies a request and returns the new parameters.
ParamValues after(const std::string& request, const ParamValues& start, Reply* replyOut = nullptr)
{
    const Reply reply = interpret(request, start);
    if (replyOut)
        *replyOut = reply;
    ParamValues p = start;
    applyEdits(p, reply.edits);
    return p;
}

bool understood(const std::string& request, const ParamValues& start)
{
    return interpret(request, start).action != Reply::Action::None;
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

void testRulesVocabulary()
{
    const auto p = midPatch();

    // Brightness.
    CHECK(v(after("make it brighter", p), param::FilterCutoffHz) > 1500.0f);
    CHECK(v(after("darker please", p), param::FilterCutoffHz) < 1500.0f);
    CHECK(v(after("less bright", p), param::FilterCutoffHz) < 1500.0f);
    CHECK(v(after("it's too bright", p), param::FilterCutoffHz) < 1500.0f);
    // "a bit" moves less than "much".
    const float bit = v(after("a bit brighter", p), param::FilterCutoffHz);
    const float lot = v(after("much brighter", p), param::FilterCutoffHz);
    CHECK(bit > 1500.0f && lot > bit);

    // With the filter already wide open, brightness comes from the EQ.
    const auto open = after("brighter", defaultValues());
    CHECK(v(open, param::EqOn) == 1.0f);
    CHECK(v(open, param::EqHighGainDb) > 0.0f);

    // Reverb.
    auto dry = defaultValues();
    const auto wet = after("add more reverb", dry);
    CHECK(v(wet, param::ReverbOn) == 1.0f);
    CHECK(v(wet, param::ReverbMix) > 0.0f);
    CHECK(v(after("more reverb", p), param::ReverbMix) > 0.3f);
    CHECK(v(after("less reverb", p), param::ReverbMix) < 0.3f);
    CHECK(v(after("turn off the reverb", p), param::ReverbOn) == 0.0f);
    CHECK(v(after("make it drier", p), param::ReverbMix) < 0.3f);
    CHECK(v(after("make it sound like a cathedral", p), param::ReverbDecaySec) > v(p, param::ReverbDecaySec));

    // Other effects and character.
    const auto delayed = after("add some delay", dry);
    CHECK(v(delayed, param::DelayOn) == 1.0f && v(delayed, param::DelayMix) > 0.0f);
    CHECK(v(after("ping pong delay", dry), param::DelayMode) == 1.0f);
    CHECK(v(after("wider", p), param::UnisonVoices) > 1.0f);
    CHECK(v(after("fatter and more detuned", p), param::UnisonDetune) > v(p, param::UnisonDetune));
    CHECK(v(after("punchier", p), param::AmpAttackMs) < 30.0f);
    CHECK(v(after("softer attack", p), param::AmpAttackMs) > 30.0f);
    CHECK(v(after("more resonance", p), param::FilterResonance) > 0.3f);
    CHECK(v(after("warmer", p), param::FilterCutoffHz) < 1500.0f);
    CHECK(v(after("louder", p), param::GainDb) > v(p, param::GainDb));
    CHECK(v(after("a bit quieter", p), param::GainDb) < v(p, param::GainDb));
    CHECK(v(after("longer release", p), param::AmpReleaseMs) > 400.0f);
    CHECK(v(after("shorter notes", p), param::AmpReleaseMs) < 400.0f);

    const auto pluck = after("make it plucky", p);
    CHECK(v(pluck, param::AmpSustain) == 0.0f && v(pluck, param::FilterEnvAmount) > 0.0f);
    const auto pad = after("turn it into a pad", p);
    CHECK(v(pad, param::AmpAttackMs) > 100.0f && v(pad, param::AmpReleaseMs) > 1000.0f);

    const auto vibrato = after("add vibrato", p);
    CHECK(v(vibrato, param::Lfo1Target) == static_cast<float>(dsp::LfoTarget::Pitch));
    CHECK(v(vibrato, param::Lfo1Depth) > 0.0f);
    CHECK(v(after("faster vibrato", vibrato), param::Lfo1RateHz) > v(vibrato, param::Lfo1RateHz));
    CHECK(v(after("add a wobble", p), param::Lfo1Target) == static_cast<float>(dsp::LfoTarget::Cutoff));
    CHECK(v(after("a slow wobble", p), param::Lfo1RateHz) < v(after("add a wobble", p), param::Lfo1RateHz));
    CHECK(v(after("fast vibrato", p), param::Lfo1RateHz) > v(after("add vibrato", p), param::Lfo1RateHz));

    const auto sub = after("add a sub", p);
    CHECK(v(sub, param::Osc3Level) > 0.0f && v(sub, param::Osc3Coarse) == -12.0f);
    const auto down = after("octave down", p);
    CHECK(v(down, param::Osc1Coarse) == -12.0f && v(down, param::Osc2Coarse) == -12.0f);

    // Several requests at once.
    const auto both = after("brighter and add some delay, then a little more resonance", p);
    CHECK(v(both, param::FilterCutoffHz) > 1500.0f);
    CHECK(v(both, param::DelayOn) == 1.0f);
    CHECK(v(both, param::FilterResonance) > 0.3f);

    // Vague words aren't "exact" (a language model may do better with them).
    Reply reply;
    after("make it brighter", p, &reply);
    CHECK(!reply.exact);
    CHECK(!reply.text.empty());

    CHECK(!understood("purple monkey dishwasher", p));

    // Several sound words in one clause all count.
    const auto warmPad = after("a warm analog pad", p);
    CHECK(v(warmPad, param::FilterCutoffHz) < 1500.0f);
    CHECK(v(warmPad, param::AmpAttackMs) > 100.0f);
    CHECK(v(after("too boomy", p), param::EqLowGainDb) < 0.0f);

    // Whether the rules understood every word (if not, a language model gets a go).
    CHECK(interpret("make it a bit brighter please", p).complete);
    CHECK(interpret("cutoff 2 kHz", p).complete);
    CHECK(interpret("warm analog pad", p).complete);
    CHECK(!interpret("brighter like a cheap 80s organ", p).complete);
    CHECK(!interpret("it should feel underwater", p).complete);
}

void testRulesExact()
{
    const auto p = midPatch();
    Reply reply;

    CHECK(v(after("set the cutoff to 2 kHz", p, &reply), param::FilterCutoffHz) == 2000.0f);
    CHECK(reply.exact);
    CHECK(v(after("cutoff 800", p), param::FilterCutoffHz) == 800.0f);
    CHECK(v(after("attack 20 ms", p), param::AmpAttackMs) == 20.0f);
    CHECK(v(after("release 1.5 s", p), param::AmpReleaseMs) == 1500.0f);
    CHECK(v(after("filter attack 5ms", p), param::FilterAttackMs) == 5.0f);
    CHECK_NEAR(v(after("reverb mix 40%", p), param::ReverbMix), 0.4, 1e-6);
    CHECK_NEAR(v(after("reverb mix 40", p), param::ReverbMix), 0.4, 1e-6);
    CHECK(v(after("unison 5 voices", p), param::UnisonVoices) == 5.0f);
    CHECK(v(after("gain -6 db", p), param::GainDb) == -6.0f);
    CHECK(v(after("osc 2 coarse 7", p), param::Osc2Coarse) == 7.0f);
    CHECK(v(after("raise the cutoff by 500 hz", p), param::FilterCutoffHz) == 2000.0f);
    CHECK(v(after("lower the gain by 3 db", p), param::GainDb) == v(p, param::GainDb) - 3.0f);
    CHECK(v(after("resonance max", p), param::FilterResonance) == 1.0f);
    CHECK(v(after("double the release", p), param::AmpReleaseMs) == 800.0f);

    // Setting a parameter of an effect that is off switches the effect on.
    const auto d = after("delay time 250 ms", defaultValues());
    CHECK(v(d, param::DelayTimeMs) == 250.0f && v(d, param::DelayOn) == 1.0f);

    // Named choices.
    CHECK(v(after("high pass filter", p), param::FilterType) == static_cast<float>(dsp::FilterType::HighPass));
    CHECK(v(after("osc 1 square", p), param::Osc1Wave) == static_cast<float>(dsp::Wave::Square));
    const auto osc2 = after("osc2 triangle", p);
    CHECK(v(osc2, param::Osc2Wave) == static_cast<float>(dsp::Wave::Triangle));
    CHECK(v(osc2, param::Osc2Level) > 0.0f); // a silent oscillator is brought in
    CHECK(v(after("sine wave", p), param::Osc1Wave) == static_cast<float>(dsp::Wave::Sine));
    CHECK(v(after("delay sync 1/8", p), param::DelaySync) == 3.0f);
    CHECK(v(after("lfo 1 target cutoff", p), param::Lfo1Target) == static_cast<float>(dsp::LfoTarget::Cutoff));
    CHECK(v(after("lfo1 wave random", p), param::Lfo1Wave) == static_cast<float>(dsp::LfoWave::Random));
    CHECK(v(after("enable the eq", p), param::EqOn) == 1.0f);
    CHECK(v(after("no delay", after("add delay", p)), param::DelayOn) == 0.0f);

    // Commands.
    CHECK(interpret("undo", p).action == Reply::Action::Undo);
    CHECK(interpret("redo", p).action == Reply::Action::Redo);
    CHECK(interpret("start over", p).action == Reply::Action::Reset);
    CHECK(interpret("help", p).action == Reply::Action::Help);
    CHECK(interpret("show the current settings", p).action == Reply::Action::Show);
    CHECK(interpret("undo", p).exact);
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
    CHECK(text.find("reverb") != std::string::npos);
    CHECK(text.find("-12 st") != std::string::npos);
    CHECK(!helpText().empty());
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
    testRulesVocabulary();
    testRulesExact();
    testDescribe();
    testModelPrompt();
    testModelReply();

    if (g_failures == 0)
        std::puts("All assistant tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
