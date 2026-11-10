#pragma once

// The synth engine. Plain C++ with no plugin or GUI dependencies, so the same
// code runs inside the plugin, the render CLI and (later) Python.
//
// Real-time rules: after prepare(), nothing here allocates, locks or does I/O.
// Output is deterministic: the same parameters and note events always produce
// the same samples.
//
// Signal path per voice: 3 oscillators + noise -> mixer -> filter -> amp envelope.
// The filter's cutoff follows the note (key tracking) and its own envelope.

#include "synthmatch/Params.h"
#include "synthmatch/dsp/Envelope.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Oscillator.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace synthmatch {

class Engine {
public:
    static constexpr int kMaxVoices = 16;

    void prepare(double sampleRate);
    void reset();

    void setParam(int index, float value);
    void setParams(const ParamValues& values);
    float getParam(int index) const { return params_[static_cast<std::size_t>(index)]; }

    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff(); // every voice enters its release
    void allSoundOff(); // every voice stops immediately

    // Overwrites numSamples samples in both channels.
    void process(float* left, float* right, int numSamples);

    int activeVoiceCount() const;

private:
    struct Voice {
        dsp::AdsrState amp;
        dsp::AdsrState filterEnv;
        dsp::SvfState filter;
        int note = -1;
        float velocity = 0.0f;
        double baseHz = 0.0;
        std::array<double, param::kNumOscs> phase{}; // each in [0, 1)
        std::uint32_t noiseState = 1;
        std::uint64_t startOrder = 0;
    };

    struct OscSettings {
        dsp::Wave wave = dsp::Wave::Saw;
        double level = 0.0;
        double pitchRatio = 1.0; // from coarse and fine tuning
        double pulseWidth = 0.5;
    };

    Voice& allocateVoice();
    void updateDerived();

    double sampleRate_ = 48000.0;
    ParamValues params_ = defaultValues();
    std::array<Voice, kMaxVoices> voices_{};
    std::uint64_t noteCounter_ = 0;

    // Values derived from params_ and the sample rate.
    float gain_ = 1.0f;
    dsp::AdsrCoeffs ampEnv_;
    std::array<OscSettings, param::kNumOscs> oscs_{};
    double noiseLevel_ = 0.0;

    dsp::FilterType filterType_ = dsp::FilterType::LowPass;
    double filterCutoffHz_ = 20000.0;
    double filterDamping_ = 1.0; // 1/Q, from the resonance parameter
    double filterKeyTrack_ = 0.0;
    double filterEnvOctaves_ = 0.0;
    dsp::AdsrCoeffs filterEnv_;
};

} // namespace synthmatch
