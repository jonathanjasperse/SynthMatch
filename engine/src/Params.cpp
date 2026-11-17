#include "synthmatch/Params.h"

#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/dsp/Reverb.h"

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

// Same order as dsp::FilterType.
constexpr std::array<std::string_view, 3> kFilterNames{"lowpass", "bandpass", "highpass"};
static_assert(kFilterNames.size() == static_cast<std::size_t>(dsp::FilterType::Count));

constexpr float kLastFilter = static_cast<float>(dsp::FilterType::Count) - 1.0f;

// Same order as dsp::LfoWave and dsp::LfoTarget.
constexpr std::array<std::string_view, 5> kLfoWaveNames{"sine", "triangle", "saw", "square", "random"};
static_assert(kLfoWaveNames.size() == static_cast<std::size_t>(dsp::LfoWave::Count));
constexpr std::array<std::string_view, 5> kLfoTargetNames{"off", "pitch", "cutoff", "amp", "pw"};
static_assert(kLfoTargetNames.size() == static_cast<std::size_t>(dsp::LfoTarget::Count));

constexpr float kLastLfoWave = static_cast<float>(dsp::LfoWave::Count) - 1.0f;
constexpr float kLastLfoTarget = static_cast<float>(dsp::LfoTarget::Count) - 1.0f;

constexpr std::array<std::string_view, 2> kOffOn{"off", "on"};
constexpr std::array<std::string_view, 2> kDelayModeNames{"stereo", "pingpong"};

// Same order as kDelaySyncBeats in Params.h.
constexpr std::array<std::string_view, 8> kDelaySyncNames{"off", "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2"};
static_assert(kDelaySyncNames.size() == kDelaySyncBeats.size());
constexpr float kLastDelaySync = static_cast<float>(kDelaySyncNames.size()) - 1.0f;

// Same order as dsp::ReverbMode.
constexpr std::array<std::string_view, 5> kReverbModeNames{"room", "chamber", "hall", "plate", "vintage"};
static_assert(kReverbModeNames.size() == static_cast<std::size_t>(dsp::ReverbMode::Count));
constexpr float kLastReverbMode = static_cast<float>(dsp::ReverbMode::Count) - 1.0f;
constexpr float kHall = static_cast<float>(dsp::ReverbMode::Hall);

// Same order as param::Index. Never reorder or reuse ids: saved presets and
// trained models refer to parameters by id.
constexpr std::array<ParamSpec, kNumParams> kSchema{{
    {"gain_db", "Gain", -60.0f, 2.0f, -20.0f, Curve::Linear, "dB"},
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

    // The default filter is a fully open low-pass. Key tracking moves the
    // cutoff with the note (1 = one octave per octave, centred on middle C).
    // The envelope amount is how far the filter envelope moves the cutoff.
    {"filter_type", "Filter Type", 0.0f, kLastFilter, 0.0f, Curve::Stepped, "", kFilterNames},
    {"filter_cutoff_hz", "Filter Cutoff", 20.0f, 20000.0f, 20000.0f, Curve::Log, "Hz"},
    {"filter_resonance", "Filter Resonance", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
    {"filter_keytrack", "Filter Key Track", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
    {"filter_env_amount", "Filter Env Amount", -8.0f, 8.0f, 0.0f, Curve::Linear, "oct"},
    {"filter_attack_ms", "Filter Attack", 0.1f, 5000.0f, 5.0f, Curve::Log, "ms"},
    {"filter_decay_ms", "Filter Decay", 1.0f, 10000.0f, 300.0f, Curve::Log, "ms"},
    {"filter_sustain", "Filter Sustain", 0.0f, 1.0f, 0.5f, Curve::Linear, ""},
    {"filter_release_ms", "Filter Release", 1.0f, 10000.0f, 200.0f, Curve::Log, "ms"},

    // Unison: each oscillator becomes several copies, detuned across
    // +/- unison_detune cents and panned across the stereo field. At the most
    // detune the outer copies are a semitone apart.
    {"unison_voices", "Unison Voices", 1.0f, static_cast<float>(kMaxUnison), 1.0f, Curve::Stepped, ""},
    {"unison_detune", "Unison Detune", 0.0f, 50.0f, 10.0f, Curve::Linear, "cents"},
    {"unison_spread", "Unison Spread", 0.0f, 1.0f, 0.5f, Curve::Linear, ""},

    // LFOs. See dsp/Lfo.h for what depth means for each target.
    {"lfo1_wave", "LFO 1 Wave", 0.0f, kLastLfoWave, 0.0f, Curve::Stepped, "", kLfoWaveNames},
    {"lfo1_rate_hz", "LFO 1 Rate", 0.05f, 30.0f, 5.0f, Curve::Log, "Hz"},
    {"lfo1_target", "LFO 1 Target", 0.0f, kLastLfoTarget, 0.0f, Curve::Stepped, "", kLfoTargetNames},
    {"lfo1_depth", "LFO 1 Depth", 0.0f, 1.0f, 0.25f, Curve::Linear, ""},

    {"lfo2_wave", "LFO 2 Wave", 0.0f, kLastLfoWave, 0.0f, Curve::Stepped, "", kLfoWaveNames},
    {"lfo2_rate_hz", "LFO 2 Rate", 0.05f, 30.0f, 1.0f, Curve::Log, "Hz"},
    {"lfo2_target", "LFO 2 Target", 0.0f, kLastLfoTarget, 0.0f, Curve::Stepped, "", kLfoTargetNames},
    {"lfo2_depth", "LFO 2 Depth", 0.0f, 1.0f, 0.25f, Curve::Linear, ""},

    {"bend_range", "Pitch Bend Range", 0.0f, 24.0f, 2.0f, Curve::Stepped, "st"},

    // The adjustable EQ node: low shelf, two bells, high shelf.
    {"eq_on", "EQ", 0.0f, 1.0f, 0.0f, Curve::Stepped, "", kOffOn},
    {"eq_low_freq_hz", "EQ Low Freq", 30.0f, 1000.0f, 120.0f, Curve::Log, "Hz"},
    {"eq_low_gain_db", "EQ Low Gain", -18.0f, 18.0f, 0.0f, Curve::Linear, "dB"},
    {"eq_mid1_freq_hz", "EQ Mid 1 Freq", 100.0f, 8000.0f, 500.0f, Curve::Log, "Hz"},
    {"eq_mid1_gain_db", "EQ Mid 1 Gain", -18.0f, 18.0f, 0.0f, Curve::Linear, "dB"},
    {"eq_mid1_q", "EQ Mid 1 Q", 0.3f, 8.0f, 1.0f, Curve::Log, ""},
    {"eq_mid2_freq_hz", "EQ Mid 2 Freq", 200.0f, 16000.0f, 3000.0f, Curve::Log, "Hz"},
    {"eq_mid2_gain_db", "EQ Mid 2 Gain", -18.0f, 18.0f, 0.0f, Curve::Linear, "dB"},
    {"eq_mid2_q", "EQ Mid 2 Q", 0.3f, 8.0f, 1.0f, Curve::Log, ""},
    {"eq_high_freq_hz", "EQ High Freq", 1000.0f, 18000.0f, 8000.0f, Curve::Log, "Hz"},
    {"eq_high_gain_db", "EQ High Gain", -18.0f, 18.0f, 0.0f, Curve::Linear, "dB"},

    // Delay. With delay_sync on, the time follows the host tempo instead of
    // delay_time_ms. delay_spread makes the right channel up to 50% longer.
    {"delay_on", "Delay", 0.0f, 1.0f, 0.0f, Curve::Stepped, "", kOffOn},
    {"delay_time_ms", "Delay Time", 1.0f, 2000.0f, 350.0f, Curve::Log, "ms"},
    {"delay_sync", "Delay Sync", 0.0f, kLastDelaySync, 0.0f, Curve::Stepped, "", kDelaySyncNames},
    {"delay_feedback", "Delay Feedback", 0.0f, 0.95f, 0.35f, Curve::Linear, ""},
    {"delay_mix", "Delay Mix", 0.0f, 1.0f, 0.25f, Curve::Linear, ""},
    {"delay_mode", "Delay Mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, "", kDelayModeNames},
    {"delay_spread", "Delay Spread", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},
    {"delay_lowcut_hz", "Delay Low Cut", 20.0f, 2000.0f, 20.0f, Curve::Log, "Hz"},
    {"delay_highcut_hz", "Delay High Cut", 500.0f, 20000.0f, 20000.0f, Curve::Log, "Hz"},
    {"delay_mod_rate_hz", "Delay Mod Rate", 0.05f, 10.0f, 0.5f, Curve::Log, "Hz"},
    {"delay_mod_depth", "Delay Mod Depth", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},

    // Reverb is on by default, at a level you notice: mix 0.25 is about 10 dB
    // below the dry sound, with a 4 s tail.
    {"reverb_on", "Reverb", 0.0f, 1.0f, 1.0f, Curve::Stepped, "", kOffOn},
    {"reverb_mix", "Reverb Mix", 0.0f, 1.0f, 0.25f, Curve::Linear, ""},
    {"reverb_size", "Reverb Size", 0.0f, 1.0f, 0.5f, Curve::Linear, ""},
    {"reverb_decay_s", "Reverb Decay", 0.1f, 20.0f, 4.0f, Curve::Log, "s"},
    {"reverb_damping", "Reverb Damping", 0.0f, 1.0f, 0.4f, Curve::Linear, ""},
    {"reverb_predelay_ms", "Reverb Pre-delay", 0.0f, 200.0f, 20.0f, Curve::Linear, "ms"},
    {"reverb_width", "Reverb Width", 0.0f, 1.0f, 1.0f, Curve::Linear, ""},

    // How much of the match EQ curve to apply. The curve itself is stored in
    // the preset, not as parameters: see Preset.h.
    {"matcheq_amount", "Match EQ Amount", 0.0f, 1.0f, 1.0f, Curve::Linear, ""},

    // More of the reverb (see dsp/Reverb.h). The mode sets its structure. Mod
    // depth 1 wobbles the lines by about 1 ms (2.4 ms in vintage). Bass decay
    // is the bass's decay time as a multiple of the decay. The cuts shape what
    // goes into the reverb.
    {"reverb_mode", "Reverb Mode", 0.0f, kLastReverbMode, kHall, Curve::Stepped, "", kReverbModeNames},
    {"reverb_mod_depth", "Reverb Mod Depth", 0.0f, 1.0f, 0.35f, Curve::Linear, ""},
    {"reverb_mod_rate_hz", "Reverb Mod Rate", 0.05f, 5.0f, 0.5f, Curve::Log, "Hz"},
    {"reverb_diffusion", "Reverb Diffusion", 0.0f, 1.0f, 0.75f, Curve::Linear, ""},
    {"reverb_bass_mult", "Reverb Bass Decay", 0.5f, 2.0f, 1.0f, Curve::Log, "x"},
    {"reverb_lowcut_hz", "Reverb Low Cut", 20.0f, 1000.0f, 20.0f, Curve::Log, "Hz"},
    {"reverb_highcut_hz", "Reverb High Cut", 1000.0f, 20000.0f, 20000.0f, Curve::Log, "Hz"},
}};

// Node prefixes, in the order the node view lays them out.
constexpr std::array<std::string_view, 14> kNodeNames{"master", "osc1",  "osc2", "osc3",  "noise",  "unison", "filter",
                                                      "amp",    "lfo1",  "lfo2", "eq",    "delay",  "reverb", "matcheq"};

struct NodeSplit {
    std::string_view node;
    std::string_view field;
};

NodeSplit splitId(std::string_view id)
{
    for (const auto node : kNodeNames) {
        if (id.size() > node.size() + 1 && id.substr(0, node.size()) == node && id[node.size()] == '_')
            return {node, id.substr(node.size() + 1)};
    }
    return {kNodeNames[0], id}; // no node prefix: a master parameter
}

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

std::span<const std::string_view> nodeNames() { return kNodeNames; }

std::string_view nodeOf(int index) { return splitId(spec(index).id).node; }

std::string_view fieldOf(int index) { return splitId(spec(index).id).field; }

std::optional<int> findParam(std::string_view node, std::string_view field)
{
    for (std::size_t i = 0; i < kSchema.size(); ++i) {
        const auto split = splitId(kSchema[i].id);
        if (split.node == node && split.field == field)
            return static_cast<int>(i);
    }
    return std::nullopt;
}

} // namespace synthmatch
