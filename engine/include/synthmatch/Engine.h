#pragma once

// The synth engine. Plain C++ with no plugin or GUI dependencies, so the same
// code runs inside the plugin, the render CLI and (later) Python.
//
// Real-time rules: after prepare(), nothing here allocates, locks or does I/O.
// Output is deterministic: the same parameters and note events always produce
// the same samples.

#include "synthmatch/Params.h"

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
    enum class Stage : std::uint8_t { Idle, Attack, Sustain, Release };

    struct Voice {
        Stage stage = Stage::Idle;
        int note = -1;
        float velocity = 0.0f;
        double phase = 0.0;     // [0, 1)
        double increment = 0.0; // cycles per sample
        float env = 0.0f;
        std::uint64_t startOrder = 0;
    };

    Voice& allocateVoice();
    void updateDerived();

    double sampleRate_ = 48000.0;
    ParamValues params_ = defaultValues();
    std::array<Voice, kMaxVoices> voices_{};
    std::uint64_t noteCounter_ = 0;

    // Values derived from params_ and the sample rate.
    float gain_ = 1.0f;
    float attackStep_ = 1.0f;
    float releaseCoeff_ = 0.0f;
};

} // namespace synthmatch
