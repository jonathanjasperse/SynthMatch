#include "synthmatch/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace synthmatch {

namespace {

constexpr float kSilence = 1.0e-4f; // -80 dB: a releasing voice below this is freed

double noteToHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

} // namespace

void Engine::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
    updateDerived();
}

void Engine::reset()
{
    voices_.fill(Voice{});
    noteCounter_ = 0;
}

void Engine::setParam(int index, float value)
{
    if (index < 0 || index >= kNumParams)
        return;
    const float clamped = clampToRange(spec(index), value);
    auto& slot = params_[static_cast<std::size_t>(index)];
    if (slot == clamped)
        return;
    slot = clamped;
    updateDerived();
}

void Engine::setParams(const ParamValues& values)
{
    for (int i = 0; i < kNumParams; ++i)
        params_[static_cast<std::size_t>(i)] = clampToRange(spec(i), values[static_cast<std::size_t>(i)]);
    updateDerived();
}

void Engine::updateDerived()
{
    gain_ = std::pow(10.0f, params_[param::GainDb] / 20.0f);

    const double attackSamples = std::max(1.0, params_[param::AmpAttackMs] * 0.001 * sampleRate_);
    attackStep_ = static_cast<float>(1.0 / attackSamples);

    // Exponential decay that reaches -60 dB after the release time.
    const double releaseSamples = std::max(1.0, params_[param::AmpReleaseMs] * 0.001 * sampleRate_);
    releaseCoeff_ = static_cast<float>(std::exp(std::log(0.001) / releaseSamples));
}

Engine::Voice& Engine::allocateVoice()
{
    for (auto& v : voices_)
        if (v.stage == Stage::Idle)
            return v;

    // No free voice: steal the oldest one.
    return *std::min_element(voices_.begin(), voices_.end(),
                             [](const Voice& a, const Voice& b) { return a.startOrder < b.startOrder; });
}

void Engine::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127)
        return;
    if (velocity <= 0.0f) {
        noteOff(note);
        return;
    }

    Voice& v = allocateVoice();
    v.stage = Stage::Attack; // a stolen voice ramps up from its current level
    v.note = note;
    v.velocity = std::min(velocity, 1.0f);
    v.phase = 0.0;
    v.increment = noteToHz(note) / sampleRate_;
    v.startOrder = ++noteCounter_;
}

void Engine::noteOff(int note)
{
    for (auto& v : voices_)
        if (v.note == note && (v.stage == Stage::Attack || v.stage == Stage::Sustain))
            v.stage = Stage::Release;
}

void Engine::allNotesOff()
{
    for (auto& v : voices_)
        if (v.stage != Stage::Idle)
            v.stage = Stage::Release;
}

void Engine::allSoundOff() { voices_.fill(Voice{}); }

void Engine::process(float* left, float* right, int numSamples)
{
    if (numSamples <= 0)
        return;

    std::fill_n(left, numSamples, 0.0f);
    std::fill_n(right, numSamples, 0.0f);

    constexpr double twoPi = 2.0 * std::numbers::pi;

    for (auto& v : voices_) {
        if (v.stage == Stage::Idle)
            continue;

        const float amp = gain_ * v.velocity;

        for (int i = 0; i < numSamples; ++i) {
            switch (v.stage) {
            case Stage::Attack:
                v.env += attackStep_;
                if (v.env >= 1.0f) {
                    v.env = 1.0f;
                    v.stage = Stage::Sustain;
                }
                break;
            case Stage::Release:
                v.env *= releaseCoeff_;
                if (v.env < kSilence) {
                    v.env = 0.0f;
                    v.stage = Stage::Idle;
                }
                break;
            case Stage::Sustain:
            case Stage::Idle:
                break;
            }

            if (v.stage == Stage::Idle)
                break;

            const float s = static_cast<float>(std::sin(twoPi * v.phase)) * v.env * amp;
            v.phase += v.increment;
            if (v.phase >= 1.0)
                v.phase -= 1.0;

            left[i] += s;
            right[i] += s;
        }
    }
}

int Engine::activeVoiceCount() const
{
    return static_cast<int>(std::count_if(voices_.begin(), voices_.end(),
                                          [](const Voice& v) { return v.stage != Stage::Idle; }));
}

} // namespace synthmatch
