#pragma once

// The synth engine. Plain C++ with no plugin or GUI dependencies, so the same
// code runs inside the plugin, the command-line tools and the matcher.
//
// Real-time rules: after prepare(), nothing here allocates, locks or does I/O.
// Output is deterministic: the same parameters and note events always produce
// the same samples.
//
// Signal path:
//   per voice:  3 oscillators (with unison) + noise -> mixer -> filter -> amp envelope
//   then:       EQ -> delay -> reverb -> match EQ
// Two LFOs per voice can move the pitch, filter cutoff, level or pulse width.

#include "synthmatch/Params.h"
#include "synthmatch/dsp/Delay.h"
#include "synthmatch/dsp/Envelope.h"
#include "synthmatch/dsp/Eq.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/MatchEq.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/dsp/Oversampling.h"
#include "synthmatch/dsp/Reverb.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace synthmatch {

static_assert(kMatchEqBands == dsp::kMatchEqBands);

class Engine {
public:
    static constexpr int kMaxVoices = 16;

    // Allocates the delay and reverb buffers. Call before anything else, and
    // again when the sample rate changes.
    void prepare(double sampleRate);
    void reset(); // silences everything, including effect tails

    void setParam(int index, float value);
    void setParams(const ParamValues& values);
    float getParam(int index) const { return params_[static_cast<std::size_t>(index)]; }

    // The match EQ curve: one gain in dB per band (see dsp/MatchEq.h).
    void setMatchEq(std::span<const float> gainsDb);
    const MatchEqGains& matchEq() const { return matchEqGains_; }

    // Runs the oscillators at twice the sample rate, for far less aliasing (on
    // by default). Off is cheaper, for quick renders such as the matcher's
    // search. Set it before playing, not while notes sound.
    void setOversampling(bool on) { oversample_ = on; }

    void setTempo(double bpm);       // for tempo-synced delay; 120 by default
    void setPitchBend(float amount); // -1..1, scaled by the bend_range parameter

    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff(); // every voice enters its release
    void allSoundOff(); // every voice and effect tail stops immediately

    // Overwrites numSamples samples in both channels.
    void process(float* left, float* right, int numSamples);

    int activeVoiceCount() const;

private:
    struct Voice {
        dsp::AdsrState amp;
        dsp::AdsrState filterEnv;
        std::array<dsp::SvfState, 2> filter; // left, right
        std::array<dsp::Decimator, 2> decimator{}; // the oscillators' oversampling, left and right
        int note = -1;
        float velocity = 0.0f;
        double baseHz = 0.0;
        // Phase of every unison copy of every oscillator, each in [0, 1).
        std::array<std::array<double, kMaxUnison>, param::kNumOscs> phase{};
        std::array<double, param::kNumLfos> lfoPhase{};
        std::array<double, param::kNumLfos> lfoRandom{};
        std::uint32_t noiseState = 1;
        std::uint32_t lfoNoiseState = 1;
        std::uint64_t startOrder = 0;
    };

    struct OscSettings {
        dsp::Wave wave = dsp::Wave::Saw;
        double level = 0.0;
        double pitchRatio = 1.0; // from coarse and fine tuning
        double pulseWidth = 0.5;
    };

    struct LfoSettings {
        dsp::LfoWave wave = dsp::LfoWave::Sine;
        dsp::LfoTarget target = dsp::LfoTarget::Off;
        double increment = 0.0; // phase per sample
        double depth = 0.0;
        bool active = false;
    };

    Voice& allocateVoice();
    void updateDerived();
    void updateEffects();
    void renderVoices(float* left, float* right, int numSamples);

    double sampleRate_ = 48000.0;
    bool oversample_ = true;
    ParamValues params_ = defaultValues();
    MatchEqGains matchEqGains_{};
    double tempoBpm_ = 120.0;
    float pitchBend_ = 0.0f;

    std::array<Voice, kMaxVoices> voices_{};
    std::uint64_t noteCounter_ = 0;

    // Values derived from params_ and the sample rate.
    float gain_ = 1.0f;
    dsp::AdsrCoeffs ampEnv_;
    std::array<OscSettings, param::kNumOscs> oscs_{};
    double noiseLevel_ = 0.0;

    int unisonCount_ = 1;
    double unisonGain_ = 1.0;                       // keeps the level steady as copies are added
    std::array<double, kMaxUnison> unisonRatio_{};  // detune of each copy, as a frequency ratio
    std::array<double, kMaxUnison> unisonLeft_{};   // pan gains of each copy
    std::array<double, kMaxUnison> unisonRight_{};
    bool stereoVoices_ = false;                     // false while left and right are identical

    dsp::FilterType filterType_ = dsp::FilterType::LowPass;
    double filterCutoffHz_ = 20000.0;
    double filterDamping_ = 1.0; // 1/Q, from the resonance parameter
    double filterKeyTrack_ = 0.0;
    double filterEnvOctaves_ = 0.0;
    dsp::AdsrCoeffs filterEnv_;

    std::array<LfoSettings, param::kNumLfos> lfos_{};
    double bendRatio_ = 1.0;

    // Effects.
    bool eqOn_ = false, delayOn_ = false, reverbOn_ = false;
    dsp::ParametricEq eq_;
    dsp::StereoDelay delay_;
    dsp::Reverb reverb_;
    dsp::MatchEq matchEq_;
    bool running_ = false; // true once audio has been processed since the last reset
};

} // namespace synthmatch
