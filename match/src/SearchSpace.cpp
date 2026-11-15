#include "synthmatch/match/SearchSpace.h"

#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"

#include <algorithm>
#include <cmath>

namespace synthmatch::match {

float SearchDim::toValue(double x) const
{
    x = std::clamp(x, 0.0, 1.0);
    if (isChoice()) {
        const auto index = std::min(choices.size() - 1, static_cast<std::size_t>(x * static_cast<double>(choices.size())));
        return choices[index];
    }
    return fromNormalized(spec(param), static_cast<float>(x));
}

std::size_t SearchDim::choiceIndex(float value) const
{
    std::size_t best = 0;
    for (std::size_t i = 1; i < choices.size(); ++i)
        if (std::abs(choices[i] - value) < std::abs(choices[best] - value))
            best = i;
    return best;
}

double SearchDim::fromValue(float value) const
{
    if (isChoice())
        return (static_cast<double>(choiceIndex(value)) + 0.5) / static_cast<double>(choices.size());
    return toNormalized(spec(param), value);
}

ParamValues SearchSpace::toParams(std::span<const double> x) const
{
    ParamValues params = base;
    for (std::size_t i = 0; i < dims.size() && i < x.size(); ++i)
        params[static_cast<std::size_t>(dims[i].param)] = dims[i].toValue(x[i]);
    return params;
}

std::vector<double> SearchSpace::fromParams(const ParamValues& params) const
{
    std::vector<double> x(dims.size());
    for (std::size_t i = 0; i < dims.size(); ++i)
        x[i] = dims[i].fromValue(params[static_cast<std::size_t>(dims[i].param)]);
    return x;
}

std::size_t SearchSpace::outputSize() const
{
    std::size_t size = 0;
    for (const auto& d : dims)
        size += d.isChoice() ? d.choices.size() : 1;
    return size;
}

const SearchSpace& SearchSpace::standard()
{
    static const SearchSpace space = [] {
        SearchSpace s;
        s.base[param::GainDb] = kSearchGainDb;
        // With more than one unison voice the sound is spread out, as most
        // stacked patches are. (Width can't be heard in the mono analysis.)
        s.base[param::UnisonSpread] = 0.6f;

        const auto continuous = [&](int index, bool extended = false) { s.dims.push_back({index, {}, extended}); };
        const auto choice = [&](int index, std::vector<float> values, bool extended = false) {
            s.dims.push_back({index, std::move(values), extended});
        };
        const std::vector<float> waves{0.0f, 1.0f, 2.0f, 3.0f};
        // Oscillators 2 and 3 are tuned to musical intervals from the note:
        // octaves, fifths, and unison (for detuned pairs).
        const std::vector<float> intervals{-24.0f, -12.0f, 0.0f, 7.0f, 12.0f, 19.0f, 24.0f};

        // Oscillator 1 always plays the detected note, so its tuning isn't searched.
        choice(param::Osc1Wave, waves);
        continuous(param::Osc1Level);
        continuous(param::Osc1PulseWidth);
        for (int k = 1; k < param::kNumOscs; ++k) {
            choice(param::osc(k, param::OscWave), waves);
            continuous(param::osc(k, param::OscLevel));
            choice(param::osc(k, param::OscCoarse), intervals);
            continuous(param::osc(k, param::OscFine));
            continuous(param::osc(k, param::OscPulseWidth));
        }
        continuous(param::NoiseLevel);

        choice(param::FilterType, {0.0f, 1.0f, 2.0f});
        continuous(param::FilterCutoffHz);
        continuous(param::FilterResonance);
        continuous(param::FilterKeyTrack);
        continuous(param::FilterEnvAmount);
        continuous(param::FilterAttackMs);
        continuous(param::FilterDecayMs);
        continuous(param::FilterSustain);
        continuous(param::FilterReleaseMs);

        continuous(param::AmpAttackMs);
        continuous(param::AmpDecayMs);
        continuous(param::AmpSustain);
        continuous(param::AmpReleaseMs);

        choice(param::UnisonVoices, {1.0f, 2.0f, 3.0f, 5.0f, 7.0f}, true);
        continuous(param::UnisonDetune, true);

        choice(param::Lfo1Target, {0.0f, 1.0f, 2.0f, 3.0f, 4.0f}, true);
        choice(param::Lfo1Wave, {0.0f, 1.0f, 2.0f, 3.0f, 4.0f}, true);
        continuous(param::Lfo1RateHz, true);
        continuous(param::Lfo1Depth, true);

        choice(param::DelayOn, {0.0f, 1.0f}, true);
        continuous(param::DelayTimeMs, true);
        continuous(param::DelayFeedback, true);
        continuous(param::DelayMix, true);
        continuous(param::DelayHighCutHz, true);

        choice(param::ReverbOn, {0.0f, 1.0f}, true);
        continuous(param::ReverbMix, true);
        continuous(param::ReverbSize, true);
        continuous(param::ReverbDecaySec, true);
        continuous(param::ReverbDamping, true);
        return s;
    }();
    return space;
}

std::vector<bool> audibleDims(const SearchSpace& space, const ParamValues& p)
{
    const auto value = [&](int index) { return p[static_cast<std::size_t>(index)]; };
    const auto oscOn = [&](int k) { return value(param::osc(k, param::OscLevel)) > 0.02f; };
    const auto oscIsSquare = [&](int k) {
        return oscOn(k) && value(param::osc(k, param::OscWave)) == static_cast<float>(dsp::Wave::Square);
    };
    const bool envMoves = std::abs(value(param::FilterEnvAmount)) > 0.1f;
    const bool unisonOn = value(param::UnisonVoices) > 1.0f;
    const auto lfoTarget = static_cast<dsp::LfoTarget>(static_cast<int>(value(param::Lfo1Target)));
    const bool lfoOn = lfoTarget != dsp::LfoTarget::Off && value(param::Lfo1Depth) > 0.02f;
    const bool delayOn = value(param::DelayOn) >= 0.5f && value(param::DelayMix) > 0.02f;
    const bool reverbOn = value(param::ReverbOn) >= 0.5f && value(param::ReverbMix) > 0.02f;
    const bool ampSustains = value(param::AmpSustain) > 0.02f;

    std::vector<bool> audible(space.dims.size(), true);
    for (std::size_t i = 0; i < space.dims.size(); ++i) {
        const int index = space.dims[i].param;
        bool heard = true;

        for (int k = 0; k < param::kNumOscs; ++k) {
            if (index == param::osc(k, param::OscWave) || index == param::osc(k, param::OscCoarse) ||
                index == param::osc(k, param::OscFine))
                heard = oscOn(k);
            if (index == param::osc(k, param::OscPulseWidth))
                heard = oscIsSquare(k);
        }

        switch (index) {
        case param::FilterAttackMs:
        case param::FilterDecayMs:
        case param::FilterSustain:
        case param::FilterReleaseMs:
            heard = envMoves;
            break;
        case param::AmpDecayMs:
            heard = value(param::AmpSustain) < 0.98f;
            break;
        case param::AmpReleaseMs:
            heard = ampSustains;
            break;
        case param::UnisonDetune:
            heard = unisonOn;
            break;
        case param::Lfo1Wave:
        case param::Lfo1RateHz:
        case param::Lfo1Depth:
            heard = index == param::Lfo1Depth ? lfoTarget != dsp::LfoTarget::Off : lfoOn;
            break;
        case param::DelayTimeMs:
        case param::DelayFeedback:
        case param::DelayHighCutHz:
            heard = delayOn;
            break;
        case param::DelayMix:
            heard = value(param::DelayOn) >= 0.5f;
            break;
        case param::ReverbSize:
        case param::ReverbDecaySec:
        case param::ReverbDamping:
            heard = reverbOn;
            break;
        case param::ReverbMix:
            heard = value(param::ReverbOn) >= 0.5f;
            break;
        default:
            break;
        }
        audible[i] = heard;
    }
    return audible;
}

} // namespace synthmatch::match
