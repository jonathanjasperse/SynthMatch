#include "synthmatch/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace synthmatch {

namespace {

// Oscillators stay below this fraction of the sample rate.
constexpr double kMaxPhaseIncrement = 0.45;

// Key tracking leaves the cutoff unchanged at this note (middle C).
constexpr int kKeyTrackCentreNote = 60;

double noteToHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// xorshift32: a tiny deterministic random generator. Returns white noise in [-1, 1).
double nextNoise(std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<double>(static_cast<std::int32_t>(state)) * (1.0 / 2147483648.0);
}

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
    const auto p = [this](int index) { return static_cast<double>(params_[static_cast<std::size_t>(index)]); };

    gain_ = static_cast<float>(std::pow(10.0, p(param::GainDb) / 20.0));

    ampEnv_ = dsp::AdsrCoeffs::make(sampleRate_, p(param::AmpAttackMs), p(param::AmpDecayMs), p(param::AmpSustain),
                                    p(param::AmpReleaseMs));

    for (int k = 0; k < param::kNumOscs; ++k) {
        auto& o = oscs_[static_cast<std::size_t>(k)];
        o.wave = static_cast<dsp::Wave>(static_cast<int>(p(param::osc(k, param::OscWave))));
        o.level = p(param::osc(k, param::OscLevel));
        const double semitones = p(param::osc(k, param::OscCoarse)) + p(param::osc(k, param::OscFine)) / 100.0;
        o.pitchRatio = std::pow(2.0, semitones / 12.0);
        o.pulseWidth = p(param::osc(k, param::OscPulseWidth));
    }

    noiseLevel_ = p(param::NoiseLevel);

    filterType_ = static_cast<dsp::FilterType>(static_cast<int>(p(param::FilterType)));
    filterCutoffHz_ = p(param::FilterCutoffHz);
    filterDamping_ = dsp::resonanceToDamping(p(param::FilterResonance));
    filterKeyTrack_ = p(param::FilterKeyTrack);
    filterEnvOctaves_ = p(param::FilterEnvAmount);
    filterEnv_ = dsp::AdsrCoeffs::make(sampleRate_, p(param::FilterAttackMs), p(param::FilterDecayMs),
                                       p(param::FilterSustain), p(param::FilterReleaseMs));
}

Engine::Voice& Engine::allocateVoice()
{
    for (auto& v : voices_)
        if (!v.amp.active())
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
    if (!v.amp.active()) {
        // A free voice starts clean. A stolen one keeps its filter and
        // envelope state, so it ramps from where it is instead of jumping.
        v.filterEnv = {};
        v.filter = {};
    }
    v.amp.gateOn();
    v.filterEnv.gateOn();
    v.note = note;
    v.velocity = std::min(velocity, 1.0f);
    v.baseHz = noteToHz(note);
    v.phase.fill(0.0);
    v.startOrder = ++noteCounter_;
    // A different noise sequence per note, but the same one every render.
    v.noiseState = static_cast<std::uint32_t>(v.startOrder * 0x9E3779B9u) | 1u;
}

void Engine::noteOff(int note)
{
    for (auto& v : voices_)
        if (v.note == note && v.amp.stage != dsp::AdsrState::Stage::Release) {
            v.amp.gateOff();
            v.filterEnv.gateOff();
        }
}

void Engine::allNotesOff()
{
    for (auto& v : voices_) {
        v.amp.gateOff();
        v.filterEnv.gateOff();
    }
}

void Engine::allSoundOff() { voices_.fill(Voice{}); }

void Engine::process(float* left, float* right, int numSamples)
{
    if (numSamples <= 0)
        return;

    std::fill_n(left, numSamples, 0.0f);
    std::fill_n(right, numSamples, 0.0f);

    for (auto& v : voices_) {
        if (!v.amp.active())
            continue;

        const float amp = gain_ * v.velocity;

        std::array<double, param::kNumOscs> increment{};
        for (std::size_t k = 0; k < increment.size(); ++k)
            increment[k] = std::min(kMaxPhaseIncrement, v.baseHz * oscs_[k].pitchRatio / sampleRate_);

        // The cutoff for this voice before the envelope moves it.
        const double keyOctaves = filterKeyTrack_ * (v.note - kKeyTrackCentreNote) / 12.0;
        const double voiceCutoffHz = filterCutoffHz_ * std::exp2(keyOctaves);
        const bool envMovesCutoff = filterEnvOctaves_ != 0.0;

        // The coefficients depend only on the parameters, the note and the
        // envelope level, so the output doesn't depend on the block size.
        const auto filterCoeffs = [&](float envLevel) {
            const double hz = envMovesCutoff ? voiceCutoffHz * std::exp2(filterEnvOctaves_ * envLevel) : voiceCutoffHz;
            return dsp::SvfCoeffs::make(sampleRate_, hz, filterDamping_);
        };
        float coeffsEnvLevel = v.filterEnv.level;
        dsp::SvfCoeffs coeffs = filterCoeffs(coeffsEnvLevel);

        for (int i = 0; i < numSamples; ++i) {
            const float env = dsp::adsrTick(v.amp, ampEnv_);
            if (!v.amp.active())
                break;

            const float filterEnvLevel = dsp::adsrTick(v.filterEnv, filterEnv_);
            if (envMovesCutoff && filterEnvLevel != coeffsEnvLevel) {
                coeffsEnvLevel = filterEnvLevel;
                coeffs = filterCoeffs(coeffsEnvLevel);
            }

            double mix = 0.0;
            for (std::size_t k = 0; k < oscs_.size(); ++k) {
                const auto& o = oscs_[k];
                if (o.level > 0.0)
                    mix += o.level * dsp::oscSample(o.wave, v.phase[k], increment[k], o.pulseWidth);

                // Silent oscillators keep running so their phase stays in step.
                v.phase[k] += increment[k];
                if (v.phase[k] >= 1.0)
                    v.phase[k] -= 1.0;
            }

            if (noiseLevel_ > 0.0)
                mix += noiseLevel_ * nextNoise(v.noiseState);

            const double filtered = dsp::svfTick(v.filter, coeffs, filterType_, mix);

            const float s = static_cast<float>(filtered) * env * amp;
            left[i] += s;
            right[i] += s;
        }
    }
}

int Engine::activeVoiceCount() const
{
    return static_cast<int>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.amp.active(); }));
}

} // namespace synthmatch
