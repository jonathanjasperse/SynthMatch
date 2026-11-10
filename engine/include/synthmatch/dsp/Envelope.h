#pragma once

// ADSR envelope: linear attack, exponential decay and release.
// Coefficients are shared by all voices; each voice keeps only an AdsrState.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace synthmatch::dsp {

inline constexpr float kEnvSilence = 1.0e-4f; // -80 dB: below this an envelope is finished

struct AdsrCoeffs {
    float attackStep = 1.0f;   // added per sample during the attack
    float decayCoeff = 0.0f;   // multiplies the distance to sustain each sample
    float sustain = 1.0f;      // level held while the key is down, 0..1
    float releaseCoeff = 0.0f; // multiplies the level each sample after key-up

    // Decay and release times are the time to fall 60 dB.
    static AdsrCoeffs make(double sampleRate, double attackMs, double decayMs, double sustainLevel,
                           double releaseMs)
    {
        const auto fallCoeff = [sampleRate](double ms) {
            const double samples = std::max(1.0, ms * 0.001 * sampleRate);
            return static_cast<float>(std::exp(std::log(0.001) / samples));
        };

        AdsrCoeffs c;
        c.attackStep = static_cast<float>(1.0 / std::max(1.0, attackMs * 0.001 * sampleRate));
        c.decayCoeff = fallCoeff(decayMs);
        c.sustain = static_cast<float>(std::clamp(sustainLevel, 0.0, 1.0));
        c.releaseCoeff = fallCoeff(releaseMs);
        return c;
    }
};

struct AdsrState {
    enum class Stage : std::uint8_t { Idle, Attack, Decay, Release };

    Stage stage = Stage::Idle;
    float level = 0.0f;

    bool active() const { return stage != Stage::Idle; }

    // Starts from the current level, so retriggering a sounding voice doesn't jump.
    void gateOn() { stage = Stage::Attack; }

    void gateOff()
    {
        if (stage != Stage::Idle)
            stage = Stage::Release;
    }
};

// Advances the envelope by one sample and returns the new level.
inline float adsrTick(AdsrState& s, const AdsrCoeffs& c)
{
    switch (s.stage) {
    case AdsrState::Stage::Attack:
        s.level += c.attackStep;
        if (s.level >= 1.0f) {
            s.level = 1.0f;
            s.stage = AdsrState::Stage::Decay;
        }
        break;

    case AdsrState::Stage::Decay:
        // Approaches the sustain level and then holds it.
        s.level = c.sustain + (s.level - c.sustain) * c.decayCoeff;
        if (s.level < kEnvSilence && c.sustain < kEnvSilence) {
            s.level = 0.0f;
            s.stage = AdsrState::Stage::Idle; // decayed to silence: free the voice
        }
        break;

    case AdsrState::Stage::Release:
        s.level *= c.releaseCoeff;
        if (s.level < kEnvSilence) {
            s.level = 0.0f;
            s.stage = AdsrState::Stage::Idle;
        }
        break;

    case AdsrState::Stage::Idle:
        break;
    }
    return s.level;
}

} // namespace synthmatch::dsp
