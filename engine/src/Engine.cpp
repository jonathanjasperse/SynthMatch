#include "synthmatch/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

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

// Where each unison copy of an oscillator starts in its cycle, scattered so a
// stack doesn't begin with all its copies lined up in one loud spike. Without
// unison every oscillator starts at zero. With unison the first copy is
// scattered too: it's panned furthest left, and starting every oscillator's
// first copy together made the left channel several dB louder.
double unisonStartPhase(int osc, int copy, bool unison)
{
    if (copy == 0 && !unison)
        return 0.0;
    return dsp::wrap01(copy * 0.6180339887498949 + osc * 0.137);
}

// The glides' time constant.
constexpr double kGlideSec = 0.005;

} // namespace

void Engine::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    glideCoeff_ = 1.0 - std::exp(-1.0 / (kGlideSec * sampleRate_));
    dsp::decimatorTaps(); // computed here, not on the audio thread
    delay_.prepare(sampleRate_);
    reverb_.prepare(sampleRate_);
    reset();
    updateDerived();
}

void Engine::reset()
{
    voices_.fill(Voice{});
    noteCounter_ = 0;
    pitchBend_ = 0.0f;
    bendRatio_ = 1.0;
    eq_.reset();
    delay_.reset();
    reverb_.reset();
    matchEq_.reset();
    running_ = false;
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

void Engine::setMatchEq(std::span<const float> gainsDb)
{
    for (std::size_t band = 0; band < matchEqGains_.size(); ++band)
        matchEqGains_[band] = band < gainsDb.size() ? gainsDb[band] : 0.0f;
    matchEq_.set(sampleRate_, matchEqGains_, params_[param::MatchEqAmount]);
}

void Engine::setTempo(double bpm)
{
    const double clamped = std::clamp(bpm, 20.0, 400.0);
    if (clamped == tempoBpm_)
        return;
    tempoBpm_ = clamped;
    updateEffects();
}

void Engine::setPitchBend(float amount)
{
    pitchBend_ = std::clamp(amount, -1.0f, 1.0f);
    bendRatio_ = std::exp2(static_cast<double>(pitchBend_) * params_[param::BendRange] / 12.0);
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

    // Unison: copies are spread evenly from -detune to +detune cents, and from
    // left to right. The level is scaled so that more copies aren't louder.
    unisonCount_ = std::clamp(static_cast<int>(p(param::UnisonVoices)), 1, kMaxUnison);
    unisonGain_ = 1.0 / std::sqrt(static_cast<double>(unisonCount_));
    const double detuneCents = p(param::UnisonDetune);
    const double spread = p(param::UnisonSpread);
    stereoVoices_ = unisonCount_ > 1 && spread > 0.0;
    for (int u = 0; u < kMaxUnison; ++u) {
        const auto i = static_cast<std::size_t>(u);
        const double x = unisonCount_ > 1 && u < unisonCount_ ? 2.0 * u / (unisonCount_ - 1) - 1.0 : 0.0;
        unisonRatio_[i] = std::exp2(x * detuneCents / 1200.0);
        const double pan = x * spread;
        if (pan == 0.0) {
            unisonLeft_[i] = unisonRight_[i] = 1.0;
        } else {
            // Constant power, with the centre at exactly 1 on both sides.
            const double angle = (pan + 1.0) * std::numbers::pi / 4.0;
            unisonLeft_[i] = std::numbers::sqrt2 * std::cos(angle);
            unisonRight_[i] = std::numbers::sqrt2 * std::sin(angle);
        }
    }

    for (int j = 0; j < param::kNumLfos; ++j) {
        auto& l = lfos_[static_cast<std::size_t>(j)];
        l.wave = static_cast<dsp::LfoWave>(static_cast<int>(p(param::lfo(j, param::LfoWave))));
        l.target = static_cast<dsp::LfoTarget>(static_cast<int>(p(param::lfo(j, param::LfoTarget))));
        l.increment = p(param::lfo(j, param::LfoRateHz)) / sampleRate_;
        l.depth = p(param::lfo(j, param::LfoDepth));
        l.active = l.target != dsp::LfoTarget::Off && l.depth > 0.0;
    }

    bendRatio_ = std::exp2(static_cast<double>(pitchBend_) * p(param::BendRange) / 12.0);

    // With nothing sounding there's nothing to click: apply new levels at once.
    if (std::none_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.amp.active(); }))
        snapGlide();

    updateEffects();
}

void Engine::snapGlide()
{
    glide_.gain = gain_;
    glide_.noise = noiseLevel_;
    glide_.unisonGain = unisonGain_;
    for (std::size_t k = 0; k < glide_.level.size(); ++k)
        glide_.level[k] = oscs_[k].level;
    glide_.left = unisonLeft_;
    glide_.right = unisonRight_;
}

void Engine::updateEffects()
{
    const auto p = [this](int index) { return static_cast<double>(params_[static_cast<std::size_t>(index)]); };

    // An effect that has just been switched on starts with empty buffers.
    const bool eqOn = p(param::EqOn) >= 0.5;
    if (eqOn && !eqOn_)
        eq_.reset();
    eqOn_ = eqOn;

    dsp::ParametricEq::Settings eq;
    eq.lowHz = p(param::EqLowFreqHz);
    eq.lowGainDb = p(param::EqLowGainDb);
    eq.mid1Hz = p(param::EqMid1FreqHz);
    eq.mid1GainDb = p(param::EqMid1GainDb);
    eq.mid1Q = p(param::EqMid1Q);
    eq.mid2Hz = p(param::EqMid2FreqHz);
    eq.mid2GainDb = p(param::EqMid2GainDb);
    eq.mid2Q = p(param::EqMid2Q);
    eq.highHz = p(param::EqHighFreqHz);
    eq.highGainDb = p(param::EqHighGainDb);
    eq_.set(sampleRate_, eq);

    const bool delayOn = p(param::DelayOn) >= 0.5;
    if (delayOn && !delayOn_)
        delay_.reset();
    delayOn_ = delayOn;

    dsp::StereoDelay::Settings delay;
    const auto sync = static_cast<std::size_t>(p(param::DelaySync));
    delay.timeSec = sync > 0 && sync < kDelaySyncBeats.size() ? kDelaySyncBeats[sync] * 60.0 / tempoBpm_
                                                              : p(param::DelayTimeMs) * 0.001;
    delay.feedback = p(param::DelayFeedback);
    delay.mix = p(param::DelayMix);
    delay.pingPong = p(param::DelayMode) >= 0.5;
    delay.spread = p(param::DelaySpread);
    delay.lowCutHz = p(param::DelayLowCutHz);
    delay.highCutHz = p(param::DelayHighCutHz);
    delay.modRateHz = p(param::DelayModRateHz);
    delay.modDepth = p(param::DelayModDepth);
    // Before any audio has run (loading a preset, offline rendering) the delay
    // time is set directly. While playing, it glides to avoid a click.
    delay_.set(delay, !running_);

    const bool reverbOn = p(param::ReverbOn) >= 0.5;
    if (reverbOn && !reverbOn_)
        reverb_.reset();
    reverbOn_ = reverbOn;

    dsp::Reverb::Settings reverb;
    reverb.mix = p(param::ReverbMix);
    reverb.size = p(param::ReverbSize);
    reverb.decaySec = p(param::ReverbDecaySec);
    reverb.damping = p(param::ReverbDamping);
    reverb.predelaySec = p(param::ReverbPredelayMs) * 0.001;
    reverb.width = p(param::ReverbWidth);
    reverb_.set(reverb);

    matchEq_.set(sampleRate_, matchEqGains_, p(param::MatchEqAmount));
}

Engine::Voice& Engine::allocateVoice()
{
    for (auto& v : voices_)
        if (!v.amp.active())
            return v;

    // No free voice: take the quietest one already being released, where the
    // cut is least audible, and only when every voice is held, the oldest.
    Voice* quietest = nullptr;
    for (auto& v : voices_)
        if (v.amp.stage == dsp::AdsrState::Stage::Release && (quietest == nullptr || v.amp.level < quietest->amp.level))
            quietest = &v;
    if (quietest != nullptr)
        return *quietest;
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
    const bool stolen = v.amp.active();
    if (!stolen) {
        // A free voice starts clean, from known phases. A stolen one keeps its
        // envelope, filter and oscillator phases, so the waveform carries on
        // at the new pitch instead of jumping (which clicks at full level).
        v.filterEnv = {};
        v.filter = {};
        v.decimator = {};
        v.stereo = stereoVoices_;
        for (int k = 0; k < param::kNumOscs; ++k)
            for (int u = 0; u < kMaxUnison; ++u)
                v.phase[static_cast<std::size_t>(k)][static_cast<std::size_t>(u)] = unisonStartPhase(k, u, unisonCount_ > 1);
    }
    v.amp.gateOn();
    v.filterEnv.gateOn();
    v.note = note;
    v.velocity = std::min(velocity, 1.0f);
    v.baseHz = noteToHz(note);
    v.startOrder = ++noteCounter_;
    // A different noise sequence per note, but the same one every render.
    v.noiseState = static_cast<std::uint32_t>(v.startOrder * 0x9E3779B9u) | 1u;
    v.lfoNoiseState = static_cast<std::uint32_t>(v.startOrder * 0x85EBCA6Bu) | 1u;
    v.lfoPhase.fill(0.0);
    for (auto& held : v.lfoRandom)
        held = nextNoise(v.lfoNoiseState);
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

void Engine::allSoundOff()
{
    voices_.fill(Voice{});
    eq_.reset();
    delay_.reset();
    reverb_.reset();
    matchEq_.reset();
}

void Engine::process(float* left, float* right, int numSamples)
{
    if (numSamples <= 0)
        return;

    std::fill_n(left, numSamples, 0.0f);
    std::fill_n(right, numSamples, 0.0f);

    renderVoices(left, right, numSamples);

    if (eqOn_)
        eq_.process(left, right, numSamples);
    if (delayOn_)
        delay_.process(left, right, numSamples);
    if (reverbOn_)
        reverb_.process(left, right, numSamples);
    matchEq_.process(left, right, numSamples);

    running_ = true;
}

void Engine::renderVoices(float* left, float* right, int numSamples)
{
    if (std::none_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.amp.active(); })) {
        snapGlide();
        return;
    }
    for (int start = 0; start < numSamples; start += kChunk)
        renderVoiceChunk(left + start, right + start, std::min(kChunk, numSamples - start));
}

void Engine::renderVoiceChunk(float* left, float* right, int numSamples)
{
    const int steps = oversample_ ? 2 : 1;
    const auto& decimatorTaps = dsp::decimatorTaps();

    // Every sample's glided levels, worked out once for all the voices. A
    // value that has (all but) arrived snaps to its target, so a level glided
    // to zero really is zero.
    const auto glideTo = [this](double& value, double target) {
        value += glideCoeff_ * (target - value);
        if (std::abs(target - value) < 1.0e-7)
            value = target;
        return value;
    };
    for (int i = 0; i < numSamples; ++i) {
        const auto n = static_cast<std::size_t>(i);
        ramps_.gain[n] = glideTo(glide_.gain, gain_);
        ramps_.noise[n] = glideTo(glide_.noise, noiseLevel_);
        ramps_.unisonGain[n] = glideTo(glide_.unisonGain, unisonGain_);
        for (std::size_t k = 0; k < glide_.level.size(); ++k)
            ramps_.level[k][n] = glideTo(glide_.level[k], oscs_[k].level);
        for (std::size_t u = 0; u < glide_.left.size(); ++u) {
            ramps_.left[u][n] = glideTo(glide_.left[u], unisonLeft_[u]);
            ramps_.right[u][n] = glideTo(glide_.right[u], unisonRight_[u]);
        }
    }

    for (auto& v : voices_) {
        if (!v.amp.active())
            continue;
        if (stereoVoices_)
            v.stereo = true;

        std::array<double, param::kNumOscs> baseIncrement{};
        for (std::size_t k = 0; k < baseIncrement.size(); ++k)
            baseIncrement[k] = v.baseHz * oscs_[k].pitchRatio * bendRatio_ / sampleRate_;

        // The cutoff for this voice before the envelope and LFOs move it.
        const double keyOctaves = filterKeyTrack_ * (v.note - kKeyTrackCentreNote) / 12.0;
        const double voiceCutoffHz = filterCutoffHz_ * std::exp2(keyOctaves);
        const bool envMovesCutoff = filterEnvOctaves_ != 0.0;

        // The coefficients depend only on the parameters, the note, the
        // envelope level and the LFOs, so the output doesn't depend on the
        // block size.
        const auto filterCoeffs = [&](float envLevel, double lfoOctaves) {
            const double octaves = (envMovesCutoff ? filterEnvOctaves_ * envLevel : 0.0) + lfoOctaves;
            const double hz = octaves != 0.0 ? voiceCutoffHz * std::exp2(octaves) : voiceCutoffHz;
            return dsp::SvfCoeffs::make(sampleRate_, hz, filterDamping_);
        };
        float coeffsEnvLevel = v.filterEnv.level;
        double coeffsLfoOctaves = 0.0;
        dsp::SvfCoeffs coeffs = filterCoeffs(coeffsEnvLevel, coeffsLfoOctaves);

        for (int i = 0; i < numSamples; ++i) {
            const auto n = static_cast<std::size_t>(i);
            const float env = dsp::adsrTick(v.amp, ampEnv_);
            if (!v.amp.active())
                break;
            const float amp = static_cast<float>(ramps_.gain[n]) * v.velocity;

            const float filterEnvLevel = dsp::adsrTick(v.filterEnv, filterEnv_);

            // LFOs.
            double pitchSemitones = 0.0, cutoffOctaves = 0.0, pulseWidthOffset = 0.0;
            float ampMod = 1.0f;
            for (std::size_t j = 0; j < lfos_.size(); ++j) {
                const auto& l = lfos_[j];
                if (!l.active)
                    continue;
                const double value = dsp::lfoValue(l.wave, v.lfoPhase[j], v.lfoRandom[j]);
                v.lfoPhase[j] += l.increment;
                if (v.lfoPhase[j] >= 1.0) {
                    v.lfoPhase[j] -= 1.0;
                    v.lfoRandom[j] = nextNoise(v.lfoNoiseState);
                }
                switch (l.target) {
                case dsp::LfoTarget::Pitch:
                    pitchSemitones += dsp::kLfoPitchSemitones * l.depth * l.depth * value;
                    break;
                case dsp::LfoTarget::Cutoff:
                    cutoffOctaves += dsp::kLfoCutoffOctaves * l.depth * value;
                    break;
                case dsp::LfoTarget::Amp:
                    ampMod *= static_cast<float>(1.0 - l.depth * (0.5 - 0.5 * value));
                    break;
                case dsp::LfoTarget::PulseWidth:
                    pulseWidthOffset += dsp::kLfoPulseWidth * l.depth * value;
                    break;
                default:
                    break;
                }
            }

            if ((envMovesCutoff && filterEnvLevel != coeffsEnvLevel) || cutoffOctaves != coeffsLfoOctaves) {
                coeffsEnvLevel = filterEnvLevel;
                coeffsLfoOctaves = cutoffOctaves;
                coeffs = filterCoeffs(coeffsEnvLevel, coeffsLfoOctaves);
            }

            const double pitchRatio = pitchSemitones != 0.0 ? std::exp2(pitchSemitones / 12.0) : 1.0;

            // The oscillators, once per sample or (oversampling) twice at half
            // the step, filtered back down to one.
            std::array<double, 2> stepL{}, stepR{};
            for (int step = 0; step < steps; ++step) {
                double sL = 0.0, sR = 0.0;
                for (std::size_t k = 0; k < oscs_.size(); ++k) {
                    const auto& o = oscs_[k];
                    auto& phases = v.phase[k];
                    const double increment = baseIncrement[k] * pitchRatio / steps;

                    if (o.level > 0.0 || ramps_.level[k][n] > 0.0) {
                        const double pulseWidth =
                            pulseWidthOffset != 0.0 ? std::clamp(o.pulseWidth + pulseWidthOffset, 0.05, 0.95) : o.pulseWidth;
                        double sumL = 0.0, sumR = 0.0;
                        for (std::size_t u = 0; u < static_cast<std::size_t>(unisonCount_); ++u) {
                            const double inc = std::min(kMaxPhaseIncrement, increment * unisonRatio_[u]);
                            const double s = dsp::oscSample(o.wave, phases[u], inc, pulseWidth);
                            sumL += ramps_.left[u][n] * s;
                            sumR += ramps_.right[u][n] * s;
                            phases[u] += inc;
                            if (phases[u] >= 1.0)
                                phases[u] -= 1.0;
                        }
                        sL += ramps_.level[k][n] * ramps_.unisonGain[n] * sumL;
                        sR += ramps_.level[k][n] * ramps_.unisonGain[n] * sumR;
                    } else {
                        // Silent oscillators keep running so their phase stays in step.
                        for (std::size_t u = 0; u < static_cast<std::size_t>(unisonCount_); ++u) {
                            phases[u] += std::min(kMaxPhaseIncrement, increment * unisonRatio_[u]);
                            if (phases[u] >= 1.0)
                                phases[u] -= 1.0;
                        }
                    }
                }
                stepL[static_cast<std::size_t>(step)] = sL;
                stepR[static_cast<std::size_t>(step)] = sR;
            }
            double mixL = stepL[0], mixR = stepR[0];
            if (oversample_) {
                mixL = v.decimator[0].process(stepL[0], stepL[1], decimatorTaps);
                mixR = v.stereo ? v.decimator[1].process(stepR[0], stepR[1], decimatorTaps) : mixL;
            }

            if (ramps_.noise[n] > 0.0) {
                const double noise = ramps_.noise[n] * nextNoise(v.noiseState);
                mixL += noise;
                mixR += noise;
            }

            const double outL = dsp::svfTick(v.filter[0], coeffs, filterType_, mixL);
            const double outR = v.stereo ? dsp::svfTick(v.filter[1], coeffs, filterType_, mixR) : outL;

            left[i] += static_cast<float>(outL) * env * amp * ampMod;
            right[i] += static_cast<float>(outR) * env * amp * ampMod;
        }

        // While the voice is mono, keep the right filter (and decimator) in
        // step with the left so nothing jumps if unison spread is turned up mid-note.
        if (!v.stereo) {
            v.filter[1] = v.filter[0];
            v.decimator[1] = v.decimator[0];
        }
    }
}

int Engine::activeVoiceCount() const
{
    return static_cast<int>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.amp.active(); }));
}

} // namespace synthmatch
