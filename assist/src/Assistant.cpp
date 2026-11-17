#include "synthmatch/assist/Assistant.h"

#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/dsp/Reverb.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <optional>
#include <string>
#include <vector>

namespace synthmatch::assist {

namespace {

// A value with the words that name it. (An aggregate, not std::pair: an
// initializer_list stored in a pair would point at an array that is already gone.)
template <class T>
struct Named {
    T value;
    std::initializer_list<std::string_view> names;
};

// ---------------------------------------------------------------- text

// Lower-case, with punctuation turned into spaces. '|' marks a break between
// clauses. Numbers keep their sign, decimal point, '%' and '/' ("-6", "0.5",
// "40%", "1/8").
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

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(' ', start);
        const std::string word = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!word.empty())
            out.push_back(word);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out += ' ';
        out += p;
    }
    return out;
}

// Splits a request into clauses: "brighter and add reverb" is two requests.
std::vector<std::string> clauses(const std::string& normalised)
{
    std::vector<std::string> out;
    std::vector<std::string> current;
    const auto flush = [&] {
        if (!current.empty())
            out.push_back(join(current));
        current.clear();
    };
    for (const auto& word : words(normalised)) {
        if (word == "|" || word == "and" || word == "then" || word == "but" || word == "also" || word == "plus")
            flush();
        else
            current.push_back(word);
    }
    flush();
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

std::string without(const std::string& clause, std::string_view phrase)
{
    const std::size_t at = findPhrase(clause, phrase);
    if (at == std::string::npos)
        return clause;
    std::string out = clause.substr(0, at);
    const std::size_t after = at + phrase.size();
    if (after < clause.size())
        out += clause.substr(after);
    return join(words(out));
}

// ---------------------------------------------------------------- parameter names

struct Alias {
    std::string phrase;
    int param;
};

const std::vector<Alias>& aliases()
{
    static const std::vector<Alias> table = [] {
        std::vector<Alias> out;
        const auto add = [&](std::string_view phrase, int param) {
            const std::string p = normalise(phrase);
            if (!p.empty())
                out.push_back({p, param});
        };

        // Everyday names. Added first so that, among phrases of the same length,
        // they win: "reverb" means the reverb's mix, not its on/off switch.
        struct Extra {
            int index;
            std::initializer_list<const char*> phrases;
        };
        const Extra extra[] = {
            {param::FilterCutoffHz, {"cutoff", "cut off", "filter freq", "filter frequency", "cutoff frequency", "filter"}},
            {param::FilterResonance, {"resonance", "reso", "res", "filter q", "emphasis"}},
            {param::FilterKeyTrack, {"keytrack", "key track", "key tracking", "keyboard tracking"}},
            {param::FilterEnvAmount, {"env amount", "envelope amount", "filter env", "filter envelope", "filter envelope amount"}},
            {param::FilterType, {"filter type", "filter mode"}},
            {param::AmpAttackMs, {"attack", "attack time"}},
            {param::AmpDecayMs, {"decay", "decay time"}},
            {param::AmpSustain, {"sustain", "sustain level"}},
            {param::AmpReleaseMs, {"release", "release time"}},
            {param::GainDb, {"volume", "level", "loudness", "output", "master volume", "master gain", "output gain"}},
            {param::NoiseLevel, {"noise"}},
            {param::UnisonVoices, {"voices", "unison"}},
            {param::UnisonDetune, {"detune", "detuning"}},
            {param::UnisonSpread, {"spread", "stereo spread", "width", "stereo width", "stereo"}},
            {param::ReverbMix, {"reverb", "verb", "reverb amount", "reverb level", "reverb wet"}},
            {param::ReverbSize, {"room size", "room", "reverb room"}},
            {param::ReverbDecaySec, {"reverb time", "reverb length", "reverb tail", "tail", "decay time of the reverb"}},
            {param::ReverbDamping, {"damping"}},
            {param::ReverbPredelayMs, {"predelay", "pre delay", "reverb predelay", "reverb pre delay"}},
            {param::DelayMix, {"delay", "echo", "echoes", "delay amount", "delay level", "delay wet"}},
            {param::DelayTimeMs, {"echo time", "delay length"}},
            {param::DelayFeedback, {"feedback", "repeats", "delay repeats"}},
            {param::Lfo1RateHz, {"vibrato rate", "vibrato speed", "lfo rate", "lfo speed", "wobble rate", "wobble speed"}},
            {param::Lfo1Depth, {"vibrato depth", "vibrato amount", "lfo depth", "lfo amount", "wobble depth"}},
            {param::Lfo1Target, {"lfo target", "lfo destination"}},
            {param::Lfo1Wave, {"lfo wave", "lfo shape", "lfo waveform"}},
            {param::Osc1PulseWidth, {"pulse width", "pw"}},
            {param::Osc1Wave, {"waveform", "wave", "wave shape", "oscillator"}},
            {param::EqLowGainDb, {"bass", "low end", "lows", "low shelf", "bottom end", "bottom"}},
            {param::EqHighGainDb, {"treble", "highs", "high end", "high shelf", "top end", "air"}},
            {param::EqMid1GainDb, {"mids", "midrange", "low mids", "mid"}},
            {param::EqMid2GainDb, {"high mids", "upper mids", "presence"}},
            {param::MatchEqAmount, {"match eq", "eq match", "eq matching", "matching eq"}},
            {param::BendRange, {"bend range", "pitch bend"}},
        };
        for (const auto& [index, phrases] : extra)
            for (const char* phrase : phrases)
                add(phrase, index);

        // Every parameter answers to its id, its display name and "node field".
        for (int i = 0; i < kNumParams; ++i) {
            const auto& s = spec(i);
            add(s.id, i);
            add(s.name, i);
            std::string spaced(s.id);
            std::replace(spaced.begin(), spaced.end(), '_', ' ');
            add(spaced, i);
            add(std::string(nodeOf(i)) + " " + std::string(fieldOf(i)), i);
        }

        // Oscillators and LFOs, however they are written.
        for (int k = 0; k < param::kNumOscs; ++k) {
            const std::string n = std::to_string(k + 1);
            for (const std::string prefix : {"osc" + n, "osc " + n, "oscillator " + n, "oscillator" + n}) {
                for (const char* w : {"wave", "waveform", "shape"})
                    add(prefix + " " + w, param::osc(k, param::OscWave));
                for (const char* w : {"level", "volume"})
                    add(prefix + " " + w, param::osc(k, param::OscLevel));
                for (const char* w : {"coarse", "pitch", "semitones", "tuning", "tune"})
                    add(prefix + " " + w, param::osc(k, param::OscCoarse));
                for (const char* w : {"fine", "fine tune", "fine tuning", "detune"})
                    add(prefix + " " + w, param::osc(k, param::OscFine));
                for (const char* w : {"pw", "pulse width", "width"})
                    add(prefix + " " + w, param::osc(k, param::OscPulseWidth));
                add(prefix, param::osc(k, param::OscLevel));
            }
        }
        for (int j = 0; j < param::kNumLfos; ++j) {
            const std::string n = std::to_string(j + 1);
            for (const std::string prefix : {"lfo" + n, "lfo " + n}) {
                for (const char* w : {"wave", "waveform", "shape"})
                    add(prefix + " " + w, param::lfo(j, param::LfoWave));
                for (const char* w : {"rate", "speed", "frequency"})
                    add(prefix + " " + w, param::lfo(j, param::LfoRateHz));
                for (const char* w : {"target", "destination", "dest"})
                    add(prefix + " " + w, param::lfo(j, param::LfoTarget));
                for (const char* w : {"depth", "amount"})
                    add(prefix + " " + w, param::lfo(j, param::LfoDepth));
                add(prefix, param::lfo(j, param::LfoDepth));
            }
        }


        // Longest first, so "filter attack" wins over "attack".
        std::stable_sort(out.begin(), out.end(),
                         [](const Alias& a, const Alias& b) { return a.phrase.size() > b.phrase.size(); });
        return out;
    }();
    return table;
}

const Alias* findAlias(const std::string& clause)
{
    for (const auto& alias : aliases())
        if (has(clause, alias.phrase))
            return &alias;
    return nullptr;
}

// ---------------------------------------------------------------- numbers

struct Number {
    double value = 0.0;
    std::string unit; // "", "hz", "khz", "ms", "s", "db", "%", "st", "cents", "oct", "x"
};

std::string unitOf(const std::string& word)
{
    static const std::pair<const char*, const char*> units[] = {
        {"k", "khz"},        {"khz", "khz"},   {"hz", "hz"},       {"ms", "ms"},         {"milliseconds", "ms"},
        {"s", "s"},          {"sec", "s"},     {"secs", "s"},      {"second", "s"},      {"seconds", "s"},
        {"db", "db"},        {"decibels", "db"}, {"%", "%"},       {"percent", "%"},     {"st", "st"},
        {"semitone", "st"},  {"semitones", "st"}, {"cents", "cents"}, {"cent", "cents"}, {"oct", "oct"},
        {"octave", "oct"},   {"octaves", "oct"}, {"x", "x"},       {"times", "x"},       {"voices", ""},
        {"voice", ""},
    };
    for (const auto& [name, unit] : units)
        if (word == name)
            return unit;
    return "?";
}

std::optional<Number> findNumber(const std::string& clause)
{
    const auto list = words(clause);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string& word = list[i];
        char* end = nullptr;
        const double value = std::strtod(word.c_str(), &end);
        if (end == word.c_str() || !std::isfinite(value))
            continue;
        const std::string suffix(end);
        Number n;
        n.value = value;
        if (suffix.empty()) {
            if (i + 1 < list.size()) {
                const std::string next = unitOf(list[i + 1]);
                if (next != "?")
                    n.unit = next;
            }
        } else {
            n.unit = unitOf(suffix);
            if (n.unit == "?")
                continue; // "1/8", "2nd": not a plain number
        }
        return n;
    }
    return std::nullopt;
}

// A typed number in the parameter's own units.
float convert(int param, const Number& n)
{
    const auto& s = spec(param);
    double v = n.value;
    const bool unitRange = s.min == 0.0f && s.max <= 1.0f;

    if (n.unit == "%") {
        if (unitRange)
            return static_cast<float>(v / 100.0 * s.max);
        return fromNormalized(s, static_cast<float>(v / 100.0));
    }
    if (s.unit == "Hz" && n.unit == "khz")
        v *= 1000.0;
    else if (s.unit == "ms" && n.unit == "s")
        v *= 1000.0;
    else if (s.unit == "s" && n.unit == "ms")
        v /= 1000.0;
    else if (n.unit.empty() && unitRange && v > 1.0 && v <= 100.0)
        v /= 100.0; // "reverb mix 40" means 40%
    return static_cast<float>(v);
}

// ---------------------------------------------------------------- wording

// "More reverb" should be clearly more: each step raises the reverb 6 dB
// against the dry sound, whatever the mix was ("a bit": 3 dB, "a lot": 12 dB).
// Above a mix of 0.7 the dry sound would start to fade, so from there the tail
// grows longer and the room bigger instead. A reverb that was off comes in at
// about 27%, clearly audible.
constexpr double kReverbStepDb = 6.0;
constexpr double kReverbMaxMix = 0.7;
constexpr double kReverbStartMix = 0.27;

// How strongly to act: "a bit" halves the step, "a lot" doubles it.
double intensity(const std::string& clause)
{
    if (hasAny(clause, {"tiny", "tiny bit", "touch", "hair", "smidge", "very slightly", "ever so slightly"}))
        return 0.3;
    if (hasAny(clause, {"bit", "little", "slightly", "slight", "subtle", "subtly", "somewhat", "tad", "gently", "small"}))
        return 0.5;
    if (hasAny(clause, {"extremely", "massively", "hugely", "max", "maximum", "all the way", "as much as possible"}))
        return 3.0;
    if (hasAny(clause, {"much", "lot", "lots", "way", "very", "really", "far", "significantly", "super", "heavily",
                        "loads", "tons", "big", "seriously", "considerably"}))
        return 2.0;
    return 1.0;
}

// +1 to raise, -1 to lower, 0 if the clause doesn't say.
int direction(const std::string& clause, int param)
{
    // For a time, "faster" means shorter; for a rate, it means higher.
    const std::string_view unit = spec(param).unit;
    const bool isTime = unit == "ms" || unit == "s";

    if (hasAny(clause, {"faster", "quicker", "speed up", "snappier"}))
        return isTime ? -1 : 1;
    if (hasAny(clause, {"slower", "slow down"}))
        return isTime ? 1 : -1;
    if (hasAny(clause, {"less", "decrease", "reduce", "lower", "cut", "down", "drop", "shorter", "shorten", "quieter",
                        "close", "fewer", "smaller", "back off", "tame", "turn down", "pull back", "dial back",
                        "narrower", "weaker", "softer", "decreased", "reduced", "lowered"}))
        return -1;
    if (hasAny(clause, {"more", "increase", "raise", "higher", "boost", "up", "add", "bigger", "longer", "lengthen",
                        "louder", "open", "extra", "turn up", "crank", "push", "wider", "stronger", "deeper",
                        "increased", "raised", "boosted", "give it", "some"}))
        return 1;
    return 0;
}

// -1 when the request asks for the opposite of the word ("less bright", "too bright").
int polarity(const std::string& clause)
{
    if (has(clause, "enough") && hasAny(clause, {"not", "isnt", "aint"}))
        return 1; // "not bright enough"
    if (hasAny(clause, {"less", "too", "not so", "not as", "not that", "reduce", "decrease", "isnt as", "stop it being", "de"}))
        return -1;
    return 1;
}

// ---------------------------------------------------------------- building edits

struct Builder {
    const ParamValues& current;
    std::vector<Edit> edits;

    float value(int param) const { return current[static_cast<std::size_t>(param)]; }
    float normalised(int param) const { return toNormalized(spec(param), value(param)); }

    void set(int param, double v) { edits.push_back({param, Edit::Op::Set, static_cast<float>(v)}); }
    void add(int param, double v) { edits.push_back({param, Edit::Op::Add, static_cast<float>(v)}); }
    void scale(int param, double v) { edits.push_back({param, Edit::Op::Scale, static_cast<float>(v)}); }
    void nudge(int param, double v) { edits.push_back({param, Edit::Op::Nudge, static_cast<float>(v)}); }

    void turnOn(int onParam)
    {
        if (value(onParam) < 0.5f)
            set(onParam, 1.0);
    }
};

// The reverb's level against the dry sound, as a ratio, at a mix (the dry/wet
// law in dsp/Delay.h), and the mix for a ratio.
double wetRatio(double mix)
{
    return std::min(1.0, 2.0 * mix) / std::max(1.0e-3, std::min(1.0, 2.0 * (1.0 - mix)));
}
double mixForRatio(double ratio) { return ratio <= 1.0 ? 0.5 * ratio : 1.0 - 0.5 / ratio; }

// "More reverb" by `amount` (1 = plain, 0.5 = "a bit", 2 = "a lot"), or less
// when it is negative. Returns what was done.
std::string moreReverb(Builder& b, double amount)
{
    const bool on = b.value(param::ReverbOn) >= 0.5f;
    const double mix = b.value(param::ReverbMix);
    double next = mixForRatio(wetRatio(mix) * std::pow(10.0, kReverbStepDb * amount / 20.0));
    if (amount <= 0.0) {
        if (!on)
            return "The reverb is already off.";
        b.set(param::ReverbMix, next);
        return "Less reverb.";
    }
    if (!on || mix < 0.05) {
        b.turnOn(param::ReverbOn);
        b.set(param::ReverbMix, std::max(mix, std::clamp(kReverbStartMix + 0.05 * (amount - 1.0), 0.2, 0.4)));
        return "Reverb in.";
    }
    const double most = amount >= 3.0 ? 1.0 : kReverbMaxMix; // "as much as possible" may drown the dry sound
    if (next > most) {
        // As loud as it should go: make it bigger instead.
        b.set(param::ReverbMix, std::max(most, mix));
        b.scale(param::ReverbDecaySec, 1.0 + 0.25 * amount);
        b.nudge(param::ReverbSize, 0.1 * amount);
        return "More reverb: a longer tail and a bigger room.";
    }
    b.set(param::ReverbMix, next);
    b.scale(param::ReverbDecaySec, 1.0 + 0.15 * amount);
    return "More reverb, with a longer tail.";
}

// A parameter that belongs to a switchable effect brings the effect's switch with it.
int switchFor(int param)
{
    const auto node = nodeOf(param);
    if (node == "reverb")
        return param::ReverbOn;
    if (node == "delay")
        return param::DelayOn;
    if (node == "eq")
        return param::EqOn;
    return -1;
}

// Unison voice counts people actually use.
double nextUnison(float voices, int step)
{
    static const double ladder[] = {1.0, 3.0, 5.0, 7.0};
    int at = 0;
    for (int i = 0; i < 4; ++i)
        if (voices >= ladder[i])
            at = i;
    return ladder[std::clamp(at + step, 0, 3)];
}

// ---------------------------------------------------------------- vocabulary

enum class Word {
    Bright, Dark, Warm, Harsh, Reverb, Dry, Delay, Wide, Thin, Punchy, Soft, Long, Short, Pluck, Pad,
    Resonant, Detuned, Vibrato, Wobble, Tremolo, Loud, Quiet, Noisy, Clean, OctaveUp, OctaveDown, Sub,
    Bass, Treble, Huge
};

struct Entry {
    Word word;
    std::initializer_list<const char*> phrases;
};

const Entry kVocabulary[] = {
    {Word::Bright, {"brighter", "bright", "brighten", "brightness", "crisper", "crisp", "sharper", "more open", "open it up",
                    "open up", "clearer", "more presence", "cut through", "cuts through", "more bite", "sizzle"}},
    {Word::Dark, {"darker", "dark", "darken", "duller", "dull", "muffled", "muffle", "mellow", "mellower", "smoother",
                  "less harsh", "rounder", "round", "closed", "muted", "tame the highs", "softer tone"}},
    {Word::Warm, {"warmer", "warm", "warmth", "cosier", "cozy", "analog", "analogue", "vintage"}},
    {Word::Harsh, {"harsher", "harsh", "aggressive", "edgier", "edgy", "grittier", "gritty", "nastier", "nasty", "angrier",
                   "meaner", "biting"}},
    {Word::Reverb, {"reverby", "wetter", "wet", "spacious", "spacey", "roomy", "roomier", "ambience", "ambient",
                    "more space", "washy", "washier", "distant", "further away", "far away", "atmospheric", "dreamy",
                    "dreamier", "ethereal"}},
    {Word::Huge, {"huge reverb", "big reverb", "massive reverb", "cathedral", "cavernous", "hall", "epic"}},
    {Word::Dry, {"drier", "dryer", "dry", "closer", "upfront", "up front", "in your face", "intimate"}},
    {Word::Delay, {"echoey", "echoy", "echoing", "delayed", "slapback"}},
    {Word::Wide, {"wider", "wide", "fatter", "fat", "thicker", "thick", "bigger", "big", "huge", "massive", "lush", "lusher",
                  "fuller", "full", "supersaw", "chorus", "chorused", "richer", "rich", "denser", "stacked"}},
    {Word::Thin, {"thinner", "thin", "narrower", "narrow", "mono", "smaller", "small", "leaner", "lean", "simpler"}},
    {Word::Punchy, {"punchier", "punchy", "punch", "snappier", "snappy", "snap", "tighter", "tight", "percussive",
                    "more attack", "harder", "hard", "hits harder", "more impact", "impact", "stabbier"}},
    {Word::Soft, {"softer", "soft", "gentler", "gentle", "fade in", "fades in", "swell", "swells", "less attack",
                  "slower attack", "ease in", "smooth attack"}},
    {Word::Long, {"longer", "long", "ring out", "rings out", "sustained", "more sustain", "legato", "lingering", "linger"}},
    {Word::Short, {"shorter", "short", "staccato", "stabby", "stab", "choppy", "choppier", "clipped", "abrupt"}},
    {Word::Pluck, {"plucky", "pluckier", "pluck", "plucked", "like a pluck"}},
    {Word::Pad, {"pad", "pad like", "padlike", "like a pad", "pad sound", "slow pad"}},
    {Word::Resonant, {"resonant", "squelchy", "squelch", "squelchier", "acid", "acidic", "peaky", "nasal", "whistly", "zappy"}},
    {Word::Detuned, {"detuned", "out of tune", "chorusy", "wobbly pitch", "seasick", "drifty", "drifting"}},
    {Word::Vibrato, {"vibrato"}},
    {Word::Wobble, {"wobble", "wobbly", "wub", "wubby", "wah", "filter movement", "movement", "moving", "animated", "evolving",
                    "sweep", "sweeping"}},
    {Word::Tremolo, {"tremolo", "pulsing", "pulsating", "throbbing", "choppy volume"}},
    {Word::Loud, {"louder", "loud", "turn it up", "more volume", "hotter"}},
    {Word::Quiet, {"quieter", "quiet", "turn it down", "less volume"}},
    {Word::Noisy, {"noisier", "noisy", "breathy", "breathier", "hiss", "hissy", "airy noise", "dirtier", "dirty", "lo fi", "lofi"}},
    {Word::Clean, {"cleaner", "clean", "pure", "purer", "clear"}},
    {Word::OctaveUp, {"octave up", "an octave higher", "one octave higher", "up an octave", "up one octave", "higher octave",
                      "octave higher"}},
    {Word::OctaveDown, {"octave down", "an octave lower", "one octave lower", "down an octave", "down one octave",
                        "lower octave", "octave lower"}},
    {Word::Sub, {"sub", "sub bass", "sub oscillator", "sub osc"}},
    {Word::Bass, {"bassier", "boomier", "boomy", "beefier", "beefy", "more weight", "weightier", "heavier", "heavy", "deeper",
                  "deep", "more body", "body"}},
    {Word::Treble, {"airier", "airy", "sparkle", "sparkly", "sparklier", "shimmer", "shimmery", "shiny", "shinier", "glassy",
                    "glassier"}},
};

struct Match {
    const Entry* entry = nullptr;
    std::size_t length = 0;
};

// Every vocabulary word in a clause, in order, skipping phrases inside a longer
// one already found ("more attack" covers "attack"). Each word appears once.
std::vector<const Entry*> findWords(const std::string& clause)
{
    struct Hit {
        const Entry* entry;
        std::size_t at, length;
    };
    std::vector<Hit> hits;
    for (const auto& entry : kVocabulary)
        for (const char* phrase : entry.phrases)
            if (const std::size_t at = findPhrase(clause, phrase); at != std::string::npos)
                hits.push_back({&entry, at, std::string_view(phrase).size()});
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.length > b.length; });

    std::vector<Hit> taken;
    for (const auto& h : hits) {
        bool overlaps = false;
        for (const auto& t : taken)
            overlaps = overlaps || (h.at < t.at + t.length && t.at < h.at + h.length) || h.entry->word == t.entry->word;
        if (!overlaps)
            taken.push_back(h);
    }
    std::sort(taken.begin(), taken.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
    std::vector<const Entry*> out;
    for (const auto& t : taken)
        out.push_back(t.entry);
    return out;
}

Match findWord(const std::string& clause)
{
    Match best;
    for (const auto& entry : kVocabulary) {
        for (const char* phrase : entry.phrases) {
            const std::string_view p(phrase);
            if (p.size() > best.length && has(clause, p))
                best = {&entry, p.size()};
        }
    }
    return best;
}

// The edits for one vocabulary word. `sign` is -1 for its opposite, `amount`
// scales the step.
std::string applyWord(Word word, int sign, double amount, const std::string& clause, Builder& b)
{
    const double a = amount * sign;
    switch (word) {
    case Word::Bright:
    case Word::Dark: {
        const double step = (word == Word::Bright ? 0.12 : -0.12) * a;
        // When the filter is already wide open, brightness has to come from the EQ.
        if (step > 0.0 && b.normalised(param::FilterCutoffHz) > 0.97 &&
            b.value(param::FilterType) == static_cast<float>(dsp::FilterType::LowPass)) {
            b.turnOn(param::EqOn);
            b.add(param::EqHighGainDb, 3.0 * std::abs(a));
            return "Lifted the highs with the EQ (the filter is already fully open).";
        }
        b.nudge(param::FilterCutoffHz, step);
        return step > 0.0 ? "Opened the filter." : "Closed the filter a little.";
    }
    case Word::Warm:
        b.nudge(param::FilterCutoffHz, -0.07 * a);
        b.turnOn(param::EqOn);
        b.add(param::EqLowGainDb, 2.0 * a);
        b.add(param::EqHighGainDb, -1.5 * a);
        return a > 0.0 ? "Warmer: less top, more low end." : "Cooler: more top, less low end.";
    case Word::Harsh:
        b.nudge(param::FilterResonance, 0.1 * a);
        b.nudge(param::FilterCutoffHz, 0.06 * a);
        b.turnOn(param::EqOn);
        b.add(param::EqMid2GainDb, 2.5 * a);
        return a > 0.0 ? "More edge." : "Less edge.";
    case Word::Reverb:
    case Word::Huge: {
        const auto said = moreReverb(b, a);
        if (word == Word::Huge && a > 0.0) {
            b.nudge(param::ReverbSize, 0.25 * a);
            b.scale(param::ReverbDecaySec, 1.0 + 0.5 * a);
            return "A bigger space.";
        }
        return said;
    }
    case Word::Dry:
        moreReverb(b, -a);
        b.nudge(param::DelayMix, -0.1 * a);
        return a > 0.0 ? "Drier." : "Wetter.";
    case Word::Delay:
        if (a > 0.0) {
            b.turnOn(param::DelayOn);
            b.nudge(param::DelayMix, (b.value(param::DelayOn) < 0.5f ? 0.03 : 0.12) * a);
            if (has(clause, "slapback")) {
                b.set(param::DelayTimeMs, 110.0);
                b.set(param::DelayFeedback, 0.1);
            }
            return "More delay.";
        }
        b.nudge(param::DelayMix, 0.12 * a);
        return "Less delay.";
    case Word::Wide:
    case Word::Thin: {
        const int step = (word == Word::Wide ? 1 : -1) * sign;
        b.set(param::UnisonVoices, nextUnison(b.value(param::UnisonVoices), step));
        b.add(param::UnisonDetune, 5.0 * amount * step);
        b.nudge(param::UnisonSpread, 0.15 * amount * step);
        return step > 0 ? "Wider: more unison voices, spread further." : "Narrower: fewer unison voices.";
    }
    case Word::Punchy:
        if (a > 0.0) {
            b.nudge(param::AmpAttackMs, -0.15 * a);
            // A fast filter "blip" at the start of each note adds the punch.
            if (b.value(param::FilterEnvAmount) < 1.0f) {
                b.add(param::FilterEnvAmount, 1.2 * a);
                b.set(param::FilterAttackMs, 1.0);
                b.set(param::FilterDecayMs, 160.0);
                b.set(param::FilterSustain, 0.25);
            }
            return "Punchier: a faster attack and a quick filter snap.";
        }
        b.nudge(param::AmpAttackMs, -0.15 * a);
        b.add(param::FilterEnvAmount, 1.0 * a);
        return "Less punch.";
    case Word::Soft:
        b.nudge(param::AmpAttackMs, 0.15 * a);
        return a > 0.0 ? "A slower, softer attack." : "A faster attack.";
    case Word::Long:
    case Word::Short: {
        const double step = (word == Word::Long ? 0.12 : -0.12) * a;
        b.nudge(param::AmpReleaseMs, step);
        if (b.value(param::AmpSustain) < 0.9f)
            b.nudge(param::AmpDecayMs, step);
        return step > 0.0 ? "Longer notes." : "Shorter notes.";
    }
    case Word::Pluck:
        b.set(param::AmpAttackMs, 1.0);
        b.set(param::AmpDecayMs, 350.0);
        b.set(param::AmpSustain, 0.0);
        b.set(param::AmpReleaseMs, 250.0);
        b.set(param::FilterEnvAmount, std::max(2.5, static_cast<double>(b.value(param::FilterEnvAmount))));
        b.set(param::FilterAttackMs, 1.0);
        b.set(param::FilterDecayMs, 280.0);
        b.set(param::FilterSustain, 0.1);
        return "A pluck: instant attack, quick decay, filter closing with it.";
    case Word::Pad:
        b.set(param::AmpAttackMs, 350.0);
        b.set(param::AmpDecayMs, 800.0);
        b.set(param::AmpSustain, 0.85);
        b.set(param::AmpReleaseMs, 1500.0);
        return "A pad: slow attack, long release.";
    case Word::Resonant:
        b.nudge(param::FilterResonance, 0.15 * a);
        return a > 0.0 ? "More resonance." : "Less resonance.";
    case Word::Detuned:
        if (a > 0.0 && b.value(param::UnisonVoices) < 2.0f)
            b.set(param::UnisonVoices, 3.0);
        b.add(param::UnisonDetune, 8.0 * a);
        return a > 0.0 ? "More detune." : "Less detune.";
    case Word::Vibrato:
    case Word::Wobble:
    case Word::Tremolo: {
        const auto target = word == Word::Vibrato ? dsp::LfoTarget::Pitch
                                                  : (word == Word::Wobble ? dsp::LfoTarget::Cutoff : dsp::LfoTarget::Amp);
        const char* name = word == Word::Vibrato ? "vibrato" : (word == Word::Wobble ? "filter wobble" : "tremolo");
        bool already = b.value(param::Lfo1Target) == static_cast<float>(target);
        if (already && word == Word::Vibrato) {
            // A pitch LFO is only a vibrato if it's smooth and quick enough. A
            // slow square one (a match can use that to shift the pitch) is
            // replaced by a real vibrato rather than made deeper.
            const auto wave = static_cast<dsp::LfoWave>(static_cast<int>(b.value(param::Lfo1Wave)));
            const float rate = b.value(param::Lfo1RateHz);
            already = (wave == dsp::LfoWave::Sine || wave == dsp::LfoWave::Triangle) && rate >= 3.0f && rate <= 9.0f;
        }
        if (hasAny(clause, {"faster", "quicker", "speed up"})) {
            b.nudge(param::Lfo1RateHz, 0.1 * amount);
            return std::string("Faster ") + name + ".";
        }
        if (hasAny(clause, {"slower", "slow down"})) {
            b.nudge(param::Lfo1RateHz, -0.1 * amount);
            return std::string("Slower ") + name + ".";
        }
        if (a < 0.0) {
            if (already)
                b.nudge(param::Lfo1Depth, 0.5 * a * (word == Word::Vibrato ? 0.15 : 0.4));
            return std::string("Less ") + name + ".";
        }
        if (already) {
            b.nudge(param::Lfo1Depth, (word == Word::Vibrato ? 0.05 : 0.12) * a);
        } else {
            b.set(param::Lfo1Target, static_cast<double>(target));
            b.set(param::Lfo1Wave, static_cast<double>(dsp::LfoWave::Sine));
            // "a slow wobble", "fast vibrato".
            const double pace = hasAny(clause, {"slow", "lazy", "gentle", "gently"}) ? 0.4
                                : hasAny(clause, {"fast", "quick", "rapid"})      ? 2.2
                                                                                   : 1.0;
            b.set(param::Lfo1RateHz, pace * (word == Word::Vibrato ? 5.5 : (word == Word::Wobble ? 2.0 : 6.0)));
            b.set(param::Lfo1Depth, std::min(1.0, (word == Word::Vibrato ? 0.12 : 0.4) * amount));
        }
        return std::string("Added ") + name + ".";
    }
    case Word::Loud:
    case Word::Quiet:
        b.add(param::GainDb, (word == Word::Loud ? 3.0 : -3.0) * a);
        return (word == Word::Loud) == (a > 0.0) ? "Louder." : "Quieter.";
    case Word::Noisy:
        b.add(param::NoiseLevel, 0.08 * a);
        return a > 0.0 ? "Added noise." : "Less noise.";
    case Word::Clean:
        b.add(param::NoiseLevel, -0.1 * a);
        b.nudge(param::FilterResonance, -0.08 * a);
        return "Cleaner.";
    case Word::OctaveUp:
    case Word::OctaveDown: {
        const double semitones = (word == Word::OctaveUp ? 12.0 : -12.0) * sign;
        for (int k = 0; k < param::kNumOscs; ++k)
            b.add(param::osc(k, param::OscCoarse), semitones);
        return semitones > 0.0 ? "Up an octave." : "Down an octave.";
    }
    case Word::Sub:
        if (a > 0.0) {
            b.set(param::Osc3Wave, static_cast<double>(dsp::Wave::Sine));
            b.set(param::Osc3Coarse, -12.0);
            b.set(param::Osc3Level, std::clamp(std::max(0.6, b.value(param::Osc3Level) + 0.15 * a), 0.0, 1.0));
            return "Added a sub: oscillator 3 as a sine one octave down.";
        }
        b.add(param::Osc3Level, 0.2 * a);
        return "Less sub.";
    case Word::Bass:
        b.turnOn(param::EqOn);
        b.add(param::EqLowGainDb, 3.0 * a);
        return a > 0.0 ? "More low end." : "Less low end.";
    case Word::Treble:
        b.turnOn(param::EqOn);
        b.add(param::EqHighGainDb, 3.0 * a);
        return a > 0.0 ? "More air." : "Less air.";
    }
    return {};
}

// ---------------------------------------------------------------- choices and switches

// A waveform, filter type or other named choice mentioned in the clause.
bool applyChoice(const std::string& clause, const Alias* alias, Builder& b, std::string& said)
{
    const int aliasParam = alias ? alias->param : -1;
    const auto aliasNode = alias ? nodeOf(aliasParam) : std::string_view{};

    // Filter types.
    const Named<dsp::FilterType> filters[] = {
        {dsp::FilterType::LowPass, {"lowpass", "low pass", "lp", "lpf"}},
        {dsp::FilterType::BandPass, {"bandpass", "band pass", "bp", "bpf"}},
        {dsp::FilterType::HighPass, {"highpass", "high pass", "hp", "hpf"}},
    };
    for (const auto& [type, names] : filters) {
        if (hasAny(clause, names)) {
            b.set(param::FilterType, static_cast<double>(type));
            said = "Filter type changed.";
            return true;
        }
    }

    // Delay.
    if (hasAny(clause, {"ping pong", "pingpong"})) {
        b.turnOn(param::DelayOn);
        b.set(param::DelayMode, 1.0);
        said = "Ping-pong delay.";
        return true;
    }
    if (aliasNode == "delay" || hasAny(clause, {"sync", "synced", "note", "notes"})) {
        const auto& sync = spec(param::DelaySync);
        for (std::size_t i = 1; i < sync.choices.size(); ++i) {
            std::string name(sync.choices[i]);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (has(clause, name)) {
                b.turnOn(param::DelayOn);
                b.set(param::DelaySync, static_cast<double>(i));
                said = "Delay synced to the tempo.";
                return true;
            }
        }
    }

    // Waveforms: for the named oscillator or LFO, else oscillator 1.
    const Named<double> waves[] = {
        {0.0, {"sine", "sin", "sinewave", "sine wave"}},
        {1.0, {"triangle", "tri", "triangle wave"}},
        {2.0, {"saw", "sawtooth", "saw wave", "sawtooth wave"}},
        {3.0, {"square", "pulse", "square wave", "pulse wave"}},
    };
    const bool lfoNamed = aliasNode == "lfo1" || aliasNode == "lfo2";
    for (const auto& [value, names] : waves) {
        if (!hasAny(clause, names))
            continue;
        if (lfoNamed) {
            b.set(aliasNode == "lfo1" ? param::Lfo1Wave : param::Lfo2Wave, value);
        } else {
            int osc = 0;
            if (aliasNode == "osc2")
                osc = 1;
            else if (aliasNode == "osc3")
                osc = 2;
            b.set(param::osc(osc, param::OscWave), value);
            // Naming a wave for a silent oscillator means "use it".
            if (osc > 0 && b.value(param::osc(osc, param::OscLevel)) <= 0.0f)
                b.set(param::osc(osc, param::OscLevel), 0.7);
        }
        said = "Waveform changed.";
        return true;
    }
    if (lfoNamed && hasAny(clause, {"random", "sample and hold", "s&h", "stepped"})) {
        b.set(aliasNode == "lfo1" ? param::Lfo1Wave : param::Lfo2Wave, static_cast<double>(dsp::LfoWave::Random));
        said = "LFO shape changed.";
        return true;
    }

    // LFO targets.
    if (alias && (aliasParam == param::Lfo1Target || aliasParam == param::Lfo2Target)) {
        const Named<dsp::LfoTarget> targets[] = {
            {dsp::LfoTarget::Off, {"off", "nothing", "none"}},
            {dsp::LfoTarget::Pitch, {"pitch", "tuning"}},
            {dsp::LfoTarget::Cutoff, {"cutoff", "filter"}},
            {dsp::LfoTarget::Amp, {"amp", "volume", "level", "amplitude"}},
            {dsp::LfoTarget::PulseWidth, {"pw", "pulse width"}},
        };
        const std::string rest = without(clause, alias->phrase);
        for (const auto& [target, names] : targets) {
            if (hasAny(rest, names)) {
                b.set(aliasParam, static_cast<double>(target));
                said = "LFO target changed.";
                return true;
            }
        }
    }
    return false;
}

// "turn off the reverb", "no delay", "enable the eq".
bool applySwitch(const std::string& clause, Builder& b, std::string& said)
{
    const bool off = hasAny(clause, {"off", "disable", "disabled", "bypass", "remove", "kill", "no", "without", "get rid of",
                                     "lose", "mute", "drop the", "take off", "take away", "ditch"});
    const bool on = hasAny(clause, {"on", "enable", "enabled", "activate", "switch on", "turn on", "use"});
    if (!off && !on)
        return false;
    if (findNumber(clause))
        return false; // "reverb mix 0.3" is not a switch

    struct Target {
        std::initializer_list<std::string_view> names;
        int param;
        double offValue, onValue;
        const char* label;
    };
    const Target targets[] = {
        {{"reverb", "verb"}, param::ReverbOn, 0.0, 1.0, "Reverb"},
        {{"delay", "echo", "echoes"}, param::DelayOn, 0.0, 1.0, "Delay"},
        {{"match eq", "eq match", "eq matching"}, param::MatchEqAmount, 0.0, 1.0, "Match EQ"},
        {{"eq", "equalizer", "equaliser"}, param::EqOn, 0.0, 1.0, "EQ"},
        {{"lfo 2", "lfo2"}, param::Lfo2Target, 0.0, -1.0, "LFO 2"},
        {{"lfo", "lfo 1", "lfo1", "vibrato", "wobble", "tremolo", "modulation"}, param::Lfo1Target, 0.0, -1.0, "LFO 1"},
        {{"unison", "detune", "chorus"}, param::UnisonVoices, 1.0, 3.0, "Unison"},
        {{"noise", "hiss"}, param::NoiseLevel, 0.0, 0.2, "Noise"},
        {{"osc 2", "osc2", "oscillator 2"}, param::Osc2Level, 0.0, 0.7, "Oscillator 2"},
        {{"osc 3", "osc3", "oscillator 3", "sub"}, param::Osc3Level, 0.0, 0.7, "Oscillator 3"},
        {{"filter envelope", "filter env"}, param::FilterEnvAmount, 0.0, 2.0, "Filter envelope"},
        {{"key tracking", "keytrack", "key track"}, param::FilterKeyTrack, 0.0, 1.0, "Key tracking"},
    };
    for (const auto& t : targets) {
        if (!hasAny(clause, t.names))
            continue;
        if (off) {
            b.set(t.param, t.offValue);
            said = std::string(t.label) + " off.";
            return true;
        }
        if (t.onValue < 0.0)
            return false; // an LFO needs to be told what to do
        // Don't flatten a setting that is already on and higher.
        if (b.value(t.param) < t.onValue)
            b.set(t.param, t.onValue);
        said = std::string(t.label) + " on.";
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- reverb words

// With one of these in a request, sound words shape the reverb rather than the
// synth: "darker reverb" darkens the tail and leaves the filter alone.
const std::initializer_list<std::string_view> kReverbNouns = {
    "reverb", "reverbs", "verb", "tail", "plate", "hall", "chamber", "room reverb", "small room", "ambience", "cathedral"};

enum class ReverbWord {
    Room, Chamber, Hall, Cathedral, Plate, Vintage, // modes
    Dark, Warm, Bright, Clean, Muddy,               // tone
    Lush, Steady, Smooth, Grainy,                   // movement and texture
    Bigger, Smaller, Longer, Shorter, Wider, Mono,  // size, length and width
    Shimmer
};

const Named<ReverbWord> kReverbWords[] = {
    {ReverbWord::Room, {"room reverb", "small room", "room sound"}},
    {ReverbWord::Chamber, {"chamber"}},
    {ReverbWord::Hall, {"hall", "concert hall"}},
    {ReverbWord::Cathedral, {"cathedral", "church"}},
    {ReverbWord::Plate, {"plate"}},
    {ReverbWord::Vintage, {"vintage", "retro", "70s", "80s", "seventies", "eighties", "old school"}},
    {ReverbWord::Dark, {"darker", "dark", "darken", "duller", "dull", "murky", "murkier", "muffled", "mellow", "mellower"}},
    {ReverbWord::Warm, {"warmer", "warm", "warmth"}},
    {ReverbWord::Bright, {"brighter", "bright", "brighten", "airier", "airy", "shinier", "shiny", "sparkly", "crisper",
                          "crisp", "clearer"}},
    {ReverbWord::Clean, {"clean up the low end", "clean up", "cleaner", "clean", "tighter low end"}},
    {ReverbWord::Muddy, {"muddy", "muddier", "mud", "boomy", "boomier"}},
    {ReverbWord::Lush, {"lush", "lusher", "chorus", "chorused", "chorusy", "swirly", "swirlier", "swirling", "modulated",
                        "more movement", "movement", "wobbly", "wobble"}},
    {ReverbWord::Steady, {"steadier", "steady", "stable", "static"}},
    {ReverbWord::Smooth, {"smoother", "smooth", "denser", "dense", "thicker", "thick", "silky", "diffuse"}},
    {ReverbWord::Grainy, {"grainy", "grainier", "echoey", "echoy", "sparse", "sparser", "gritty"}},
    {ReverbWord::Bigger, {"huge", "massive", "enormous", "epic", "cavernous", "bigger", "big", "larger", "large", "giant",
                          "vast"}},
    {ReverbWord::Smaller, {"smaller", "small", "tighter", "tight", "compact"}},
    {ReverbWord::Longer, {"longer", "long", "lengthen", "extend"}},
    {ReverbWord::Shorter, {"shorter", "short", "shorten"}},
    {ReverbWord::Wider, {"wider", "wide", "stereo"}},
    {ReverbWord::Mono, {"mono", "narrower", "narrow"}},
    {ReverbWord::Shimmer, {"shimmer", "shimmery", "shimmering"}},
};

// True when a word is negated: "no chorus", "without any shimmer", "shimmer free".
bool negated(const std::string& clause, std::size_t at, std::size_t length)
{
    auto before = words(clause.substr(0, at));
    while (!before.empty() && (before.back() == "any" || before.back() == "the" || before.back() == "a" ||
                               before.back() == "more"))
        before.pop_back();
    const auto after = words(clause.substr(std::min(clause.size(), at + length)));
    if (!before.empty() && (before.back() == "no" || before.back() == "not" || before.back() == "without" ||
                            before.back() == "non"))
        return true;
    return !after.empty() && after.front() == "free";
}

// Reverb modes, tone, movement and size: "plate reverb", "darker reverb",
// "lush shimmer-free vintage hall". Only for a clause that names the reverb,
// and has no number in it ("reverb mix 40%" is a plain setting).
bool applyReverbWords(const std::string& clause, Builder& b, std::string& said)
{
    if (!hasAny(clause, kReverbNouns) || findNumber(clause))
        return false;
    if (hasAny(clause, {"off", "disable", "disabled", "bypass", "kill", "mute", "ditch"}))
        return false; // a switch: "turn the plate reverb off"

    // Every reverb word, longest first, skipping any inside a longer one.
    struct Hit {
        ReverbWord word;
        std::string_view phrase;
        std::size_t at;
        bool negated;
    };
    std::vector<Hit> hits;
    for (const auto& [word, names] : kReverbWords)
        for (const auto name : names)
            if (const std::size_t at = findPhrase(clause, name); at != std::string::npos)
                hits.push_back({word, name, at, negated(clause, at, name.size())});
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.phrase.size() > y.phrase.size(); });
    std::vector<Hit> found;
    for (const auto& h : hits) {
        bool overlaps = false;
        for (const auto& f : found)
            overlaps = overlaps || (h.at < f.at + f.phrase.size() && f.at < h.at + h.phrase.size()) || h.word == f.word;
        if (!overlaps)
            found.push_back(h);
    }
    std::sort(found.begin(), found.end(), [](const Hit& x, const Hit& y) { return x.at < y.at; });
    if (found.empty())
        return false; // "more reverb", "reverb size": the usual rules
    const auto named = [&](ReverbWord w) {
        return std::any_of(found.begin(), found.end(), [&](const Hit& h) { return h.word == w && !h.negated; });
    };

    const std::size_t first = b.edits.size();
    const double amount = intensity(clause);
    const int sign = hasAny(clause, {"remove", "lose", "take out", "get rid of"}) ? -1 : polarity(clause);
    const double a = amount * sign;
    std::vector<std::string> notes;
    const auto atLeast = [&](int param, double v) {
        if (b.value(param) < v)
            b.set(param, v);
    };
    const auto atMost = [&](int param, double v) {
        if (b.value(param) > v)
            b.set(param, v);
    };

    // The mode: the most particular one named ("vintage hall" is vintage, sized like a hall).
    std::optional<dsp::ReverbMode> mode;
    if (named(ReverbWord::Vintage))
        mode = dsp::ReverbMode::Vintage;
    else if (named(ReverbWord::Plate))
        mode = dsp::ReverbMode::Plate;
    else if (named(ReverbWord::Chamber))
        mode = dsp::ReverbMode::Chamber;
    else if (named(ReverbWord::Room))
        mode = dsp::ReverbMode::Room;
    else if (named(ReverbWord::Hall) || named(ReverbWord::Cathedral))
        mode = dsp::ReverbMode::Hall;
    if (mode) {
        b.set(param::ReverbMode, static_cast<double>(*mode));
        notes.push_back(std::string(choiceName(spec(param::ReverbMode), static_cast<float>(*mode))) + " reverb");
    }
    if (named(ReverbWord::Hall)) {
        atLeast(param::ReverbSize, 0.6);
        atLeast(param::ReverbDecaySec, 2.5);
    }
    if (named(ReverbWord::Cathedral)) {
        atLeast(param::ReverbSize, 0.85);
        atLeast(param::ReverbDecaySec, 4.5);
        atLeast(param::ReverbMix, 0.35);
    }
    if (named(ReverbWord::Room)) {
        atMost(param::ReverbSize, 0.5);
        atMost(param::ReverbDecaySec, 1.2);
    }
    if (named(ReverbWord::Plate)) {
        const float decay = b.value(param::ReverbDecaySec);
        if (decay < 1.2f || decay > 3.5f)
            b.set(param::ReverbDecaySec, 2.2);
    }
    if (named(ReverbWord::Vintage)) {
        // Dark, with a slow, deep chorus in the tail.
        atLeast(param::ReverbDamping, 0.5);
        atMost(param::ReverbHighCutHz, 9000.0);
        atLeast(param::ReverbModDepth, 0.5);
    }

    for (const auto& h : found) {
        // Negated words are skipped, except that "no chorus" means steadier.
        if (h.negated && h.word != ReverbWord::Lush)
            continue;
        const double s = h.negated ? -amount : a;
        switch (h.word) {
        case ReverbWord::Dark:
        case ReverbWord::Warm:
        case ReverbWord::Bright: {
            const double t = h.word == ReverbWord::Bright ? s : -s; // + is brighter
            b.nudge(param::ReverbHighCutHz, 0.15 * t);
            b.nudge(param::ReverbDamping, -0.15 * t);
            if (h.word == ReverbWord::Warm)
                b.scale(param::ReverbBassMult, 1.0 + 0.15 * s);
            notes.push_back(t > 0.0 ? "brighter" : "darker");
            break;
        }
        case ReverbWord::Clean:
        case ReverbWord::Muddy:
            if ((h.word == ReverbWord::Clean) == (s > 0.0)) {
                atLeast(param::ReverbLowCutHz, 200.0);
                b.scale(param::ReverbBassMult, 0.8);
                notes.push_back("less muddy");
            } else {
                b.nudge(param::ReverbLowCutHz, -0.2 * std::abs(s));
                b.scale(param::ReverbBassMult, 1.0 + 0.25 * std::abs(s));
                notes.push_back("fuller lows");
            }
            break;
        case ReverbWord::Lush:
        case ReverbWord::Steady: {
            const double t = h.word == ReverbWord::Lush ? s : -s;
            b.nudge(param::ReverbModDepth, 0.2 * t);
            notes.push_back(t > 0.0 ? "more movement in the tail" : "a steadier tail");
            break;
        }
        case ReverbWord::Smooth:
        case ReverbWord::Grainy: {
            const double t = h.word == ReverbWord::Smooth ? s : -s;
            b.nudge(param::ReverbDiffusion, t > 0.0 ? 0.15 * t : 0.2 * t);
            notes.push_back(t > 0.0 ? "smoother" : "grainier");
            break;
        }
        case ReverbWord::Bigger:
        case ReverbWord::Smaller: {
            const double t = h.word == ReverbWord::Bigger ? s : -s;
            b.nudge(param::ReverbSize, 0.25 * t);
            b.scale(param::ReverbDecaySec, t > 0.0 ? 1.0 + 0.4 * t : 1.0 / (1.0 - 0.4 * t));
            if (t > 0.0) {
                moreReverb(b, 0.5 * t);
                // A huge space isn't a room.
                const auto now = static_cast<dsp::ReverbMode>(static_cast<int>(b.value(param::ReverbMode)));
                if (!mode && (now == dsp::ReverbMode::Room || now == dsp::ReverbMode::Chamber))
                    b.set(param::ReverbMode, static_cast<double>(dsp::ReverbMode::Hall));
            }
            notes.push_back(t > 0.0 ? "a bigger space" : "a smaller space");
            break;
        }
        case ReverbWord::Longer:
        case ReverbWord::Shorter: {
            const double t = h.word == ReverbWord::Longer ? s : -s;
            b.scale(param::ReverbDecaySec, t > 0.0 ? 1.0 + 0.5 * t : 1.0 / (1.0 - 0.5 * t));
            notes.push_back(t > 0.0 ? "a longer tail" : "a shorter tail");
            break;
        }
        case ReverbWord::Wider:
            b.nudge(param::ReverbWidth, 0.3 * s);
            notes.push_back(s > 0.0 ? "wider" : "narrower");
            break;
        case ReverbWord::Mono:
            if (h.phrase == "mono")
                b.set(param::ReverbWidth, 0.0);
            else
                b.nudge(param::ReverbWidth, -0.3 * s);
            notes.push_back("narrower");
            break;
        case ReverbWord::Shimmer:
            if (s < 0.0) {
                b.nudge(param::ReverbHighCutHz, -0.15 * amount);
                notes.push_back("darker");
                break;
            }
            // There's no pitch-shifted (shimmer) reverb: the nearest is a bright plate.
            if (!mode)
                b.set(param::ReverbMode, static_cast<double>(dsp::ReverbMode::Plate));
            b.nudge(param::ReverbHighCutHz, 0.15 * amount);
            b.nudge(param::ReverbDamping, -0.15 * amount);
            notes.push_back("there's no shimmer (pitch-shifted) reverb, so this is a bright plate, the nearest thing");
            break;
        default:
            break; // modes, done above
        }
    }
    if (b.edits.size() == first || notes.empty())
        return false;

    // Shaping a reverb that's off brings it in.
    const bool switched = std::any_of(b.edits.begin() + static_cast<std::ptrdiff_t>(first), b.edits.end(),
                                      [](const Edit& e) { return e.param == param::ReverbOn; });
    if (!switched && b.value(param::ReverbOn) < 0.5f) {
        b.set(param::ReverbOn, 1.0);
        atLeast(param::ReverbMix, 0.25);
    }
    std::string text;
    for (const auto& n : notes)
        text += (text.empty() ? "" : ", ") + n;
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    said = text + ".";
    return true;
}

// Every word the reverb rules know, for knownWord().
std::vector<std::string_view> reverbWordList()
{
    std::vector<std::string_view> out(kReverbNouns);
    for (const auto& [word, names] : kReverbWords)
        out.insert(out.end(), names.begin(), names.end());
    return out;
}

// A clause of nothing but reverb words, right after one about the reverb,
// is about the reverb too: "make the reverb darker and lusher".
bool continuesReverb(const std::string& clause)
{
    static const std::set<std::string> allowed = [] {
        std::set<std::string> s;
        for (const auto& [word, names] : kReverbWords)
            for (const auto name : names)
                if (name.find(' ') == std::string_view::npos)
                    s.insert(std::string(name));
        for (const char* w : {"a", "bit", "little", "lot", "much", "more", "less", "even", "slightly", "very", "too", "way",
                              "tad", "touch", "lots"})
            s.insert(w);
        return s;
    }();
    const auto list = words(clause);
    return !list.empty() && std::all_of(list.begin(), list.end(), [](const std::string& w) { return allowed.count(w) > 0; });
}

// ---------------------------------------------------------------- one clause

struct ClauseResult {
    bool understood = false;
    bool exact = false;
    Reply::Action action = Reply::Action::Edit;
    std::string said;
};

ClauseResult handle(const std::string& clause, Builder& b)
{
    ClauseResult result;

    // Commands.
    const auto list = words(clause);
    const auto command = [&](Reply::Action action) {
        result.understood = result.exact = true;
        result.action = action;
        return result;
    };
    if (hasAny(clause, {"undo", "go back", "revert that", "undo that", "put it back", "that was worse", "change it back"}))
        return command(Reply::Action::Undo);
    if (hasAny(clause, {"redo", "do that again"}))
        return command(Reply::Action::Redo);
    if (hasAny(clause, {"reset", "start over", "start again", "initialise", "initialize", "init", "default preset", "from scratch"}))
        return command(Reply::Action::Reset);
    if (list.size() <= 3 && hasAny(clause, {"help", "commands", "what can you do", "what can i say", "examples"}))
        return command(Reply::Action::Help);
    if (hasAny(clause, {"show", "describe", "what is the preset", "whats the preset", "current settings", "show settings",
                        "list settings", "status", "what are the settings", "whats it set to"}))
        return command(Reply::Action::Show);

    const Alias* alias = findAlias(clause);
    const Match word = findWord(clause);
    const auto number = alias ? findNumber(without(clause, alias->phrase)) : std::nullopt;

    // Reverb words, before the switches ("reverb with no chorus" keeps the
    // reverb on) and the general words ("darker reverb" isn't a darker synth).
    if (applyReverbWords(clause, b, result.said)) {
        result.understood = true;
        return result;
    }

    // Switches.
    if (applySwitch(clause, b, result.said)) {
        result.understood = result.exact = true;
        return result;
    }

    // A vocabulary phrase at least as long as the parameter name beside it:
    // "more attack" means punchier, not a longer attack time, and "softer attack"
    // means a slower one.
    const auto applyWords = [&] {
        for (const auto* entry : findWords(clause)) {
            const auto said = applyWord(entry->word, polarity(clause), intensity(clause), clause, b);
            if (!said.empty())
                result.said += (result.said.empty() ? "" : " ") + said;
        }
        result.understood = true;
        return result;
    };
    if (word.entry && !number && (!alias || word.length >= alias->phrase.size()))
        return applyWords();

    // Named choices: waveforms, filter types, sync values.
    if (applyChoice(clause, alias, b, result.said)) {
        result.understood = result.exact = true;
        return result;
    }

    if (alias) {
        const int index = alias->param;
        const auto& s = spec(index);
        const std::string rest = without(clause, alias->phrase);
        const int dir = direction(rest, index);

        // A choice named for this parameter ("delay mode stereo", "lfo 1 target cutoff").
        if (!s.choices.empty()) {
            for (std::size_t c = 0; c < s.choices.size(); ++c) {
                std::string name(s.choices[c]);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (has(rest, name)) {
                    b.set(index, s.min + static_cast<double>(c));
                    result.understood = result.exact = true;
                    return result;
                }
            }
        }

        const auto enable = [&](bool raising) {
            const int sw = switchFor(index);
            if (sw >= 0 && sw != index && raising)
                b.turnOn(sw);
        };

        if (number) {
            const float value = convert(index, *number);
            if (has(rest, "by") && dir != 0) {
                // "raise the cutoff by 500 Hz", "lower the gain by 3 dB", "by 20%".
                if (number->unit == "%" && !(s.min == 0.0f && s.max <= 1.0f))
                    b.scale(index, 1.0 + dir * number->value / 100.0);
                else if (number->unit == "x")
                    b.scale(index, dir > 0 ? number->value : 1.0 / std::max(number->value, 1.0e-3));
                else
                    b.add(index, dir * std::abs(value));
                enable(dir > 0);
            } else {
                b.set(index, value);
                enable(true);
            }
            result.understood = result.exact = true;
            return result;
        }

        // "max", "minimum", "half".
        if (hasAny(rest, {"max", "maximum", "full", "all the way up", "fully open", "wide open"})) {
            b.set(index, s.max);
            enable(true);
            result.understood = result.exact = true;
            return result;
        }
        if (hasAny(rest, {"min", "minimum", "zero", "none", "all the way down", "fully closed"})) {
            b.set(index, has(rest, "zero") || has(rest, "none") ? std::max(0.0f, s.min) : s.min);
            result.understood = result.exact = true;
            return result;
        }
        if (hasAny(rest, {"half", "halfway", "middle", "medium"})) {
            b.set(index, fromNormalized(s, 0.5f));
            enable(true);
            result.understood = result.exact = true;
            return result;
        }
        if (hasAny(rest, {"double", "twice"})) {
            b.scale(index, 2.0);
            enable(true);
            result.understood = result.exact = true;
            return result;
        }
        if (hasAny(rest, {"halve", "half as"})) {
            b.scale(index, 0.5);
            result.understood = result.exact = true;
            return result;
        }

        // "more reverb", "lower the cutoff a bit", "longer release".
        if (dir != 0) {
            // "More reverb" and "less reverb" have their own, bigger steps.
            if (index == param::ReverbMix) {
                result.said = moreReverb(b, dir * intensity(rest));
                result.understood = true;
                return result;
            }
            const double step = 0.1 * intensity(rest) * dir;
            enable(dir > 0);
            // Bringing in a delay that was off starts from a modest level.
            const int sw = switchFor(index);
            if (dir > 0 && sw >= 0 && b.value(sw) < 0.5f && index == param::DelayMix)
                b.nudge(index, 0.3 * step);
            else
                b.nudge(index, step);
            result.understood = true;
            return result;
        }
    }

    // Plain vocabulary words.
    if (word.entry && !number)
        return applyWords();

    return result;
}

// Words the rules understand, or can safely ignore ("make", "it", "a bit").
// A request containing anything else may mean more than the rules can see.
bool knownWord(const std::string& word)
{
    static const std::set<std::string> known = [] {
        std::set<std::string> s;
        const auto addWords = [&](std::string_view phrase) {
            for (const auto& w : words(normalise(phrase)))
                s.insert(w);
        };
        for (const auto& entry : kVocabulary)
            for (const char* phrase : entry.phrases)
                addWords(phrase);
        for (const auto& alias : aliases())
            addWords(alias.phrase);
        for (int i = 0; i < kNumParams; ++i)
            for (const auto choice : spec(i).choices)
                addWords(choice);
        for (const auto phrase : reverbWordList())
            addWords(phrase);
        for (const char* w : {
                 // filler
                 "make", "it", "its", "the", "a", "an", "and", "to", "of", "on", "in", "for", "with", "some", "please", "can",
                 "could", "you", "i", "want", "need", "would", "like", "be", "is", "are", "should", "sound", "sounds", "feel",
                 "this", "that", "me", "my", "just", "so", "very", "really", "too", "quite", "kind", "sort", "bit", "little",
                 "lot", "lots", "much", "way", "more", "less", "slightly", "tad", "touch", "hair", "abit", "now", "again",
                 "also", "then", "but", "plus", "not", "no", "isnt", "enough", "than", "by", "at", "from", "into", "as",
                 "give", "get", "add", "put", "turn", "set", "change", "go", "use", "try", "keep", "maybe", "about", "around",
                 "do", "dont", "thing", "one", "overall", "whole", "synth", "patch", "preset", "tone", "note", "notes",
                 // directions and amounts
                 "up", "down", "increase", "decrease", "raise", "lower", "reduce", "boost", "cut", "higher", "faster",
                 "slower", "quicker", "longer", "shorter", "louder", "quieter", "bigger", "smaller", "stronger", "weaker",
                 "extra", "max", "maximum", "min", "minimum", "half", "double", "twice", "zero", "none", "all", "full",
                 "off", "enable", "disable", "remove", "kill", "bypass", "without", "drop", "open", "close", "push", "crank",
                 "tame", "back", "dial", "pull", "speed", "slow", "extremely", "massively", "hugely", "significantly",
                 "super", "heavily", "loads", "tons", "seriously", "considerably", "subtle", "subtly", "somewhat", "gently",
                 "tiny", "smidge", "ever", "small", "big", "free", "any",
                 // units
                 "hz", "khz", "k", "ms", "s", "sec", "secs", "second", "seconds", "db", "percent", "st", "semitone",
                 "semitones", "cent", "cents", "oct", "octave", "octaves", "x", "times", "voice", "voices"})
            s.insert(w);
        return s;
    }();

    if (known.count(word) != 0 || word.find('/') != std::string::npos)
        return true;
    char* end = nullptr;
    std::strtod(word.c_str(), &end);
    return end != word.c_str(); // a number, maybe with a unit ("2k", "40%")
}

bool allWordsKnown(const std::string& normalised)
{
    for (const auto& w : words(normalised))
        if (w != "|" && !knownWord(w))
            return false;
    return true;
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

Reply interpret(std::string_view request, const ParamValues& current)
{
    Reply reply;
    Builder builder{current, {}};
    bool allExact = true, anyUnderstood = false, anyMissed = false;
    std::string said;

    bool aboutReverb = false; // the last clause named the reverb
    for (const auto& clause : clauses(normalise(request))) {
        const bool continues = aboutReverb && continuesReverb(clause);
        aboutReverb = continues || hasAny(clause, kReverbNouns);
        const ClauseResult result = handle(continues ? clause + " reverb" : clause, builder);
        if (!result.understood) {
            anyMissed = true;
            continue;
        }
        anyUnderstood = true;
        allExact = allExact && result.exact;
        if (result.action != Reply::Action::Edit) {
            // A command stands alone: "undo" doesn't combine with edits.
            reply.action = result.action;
            reply.exact = reply.complete = true;
            return reply;
        }
        if (!result.said.empty()) {
            if (!said.empty())
                said += " ";
            said += result.said;
        }
    }

    if (!anyUnderstood || builder.edits.empty()) {
        reply.action = Reply::Action::None;
        reply.text = "I didn't catch that. Try \"make it brighter\", \"more reverb\", \"set the cutoff to 2 kHz\" or \"help\".";
        return reply;
    }

    reply.action = Reply::Action::Edit;
    reply.edits = std::move(builder.edits);
    reply.text = said;
    reply.exact = allExact && !anyMissed;
    reply.complete = !anyMissed && allWordsKnown(normalise(request));
    return reply;
}

std::string helpText()
{
    return "Say what you want to hear, or name a setting:\n"
           "  make it brighter / darker / warmer / wider / punchier / softer\n"
           "  add more reverb    less delay    a bit more resonance\n"
           "  plate / hall / room / vintage reverb    darker reverb    lush reverb\n"
           "  make it plucky     turn it into a pad    add vibrato    add a sub\n"
           "  set the cutoff to 2 kHz    attack 20 ms    reverb mix 40%\n"
           "  osc 2 square       unison 5 voices       high pass filter\n"
           "  turn off the delay    octave down    louder\n"
           "  show (the current settings)    undo    redo    reset";
}

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
