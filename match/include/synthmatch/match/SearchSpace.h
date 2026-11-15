#pragma once

// The part of the parameter space the matcher explores, as a list of
// dimensions that each run from 0 to 1. The search, the trained encoder and
// the training-data generator all share this one description.
//
// Parameters not listed here keep fixed values while matching (for example the
// adjustable EQ node, which is left free for the user, and stereo settings,
// which can't be heard in the mono analysis).

#include "synthmatch/Params.h"

#include <cstddef>
#include <span>
#include <vector>

namespace synthmatch::match {

struct SearchDim {
    int param = 0;
    // Empty: a continuous dimension covering the parameter's whole range, using
    // its curve. Otherwise: the only values this parameter may take.
    std::vector<float> choices;
    // Extended dimensions (unison, LFO, delay, reverb) are searched after the
    // core sound (oscillators, filter, envelopes) has been found.
    bool extended = false;

    bool isChoice() const { return !choices.empty(); }
    float toValue(double x) const;        // x in [0, 1]
    double fromValue(float value) const;  // the middle of the matching choice's slot
    std::size_t choiceIndex(float value) const; // nearest choice
};

struct SearchSpace {
    std::vector<SearchDim> dims;
    ParamValues base = defaultValues(); // everything not searched

    ParamValues toParams(std::span<const double> x) const;
    std::vector<double> fromParams(const ParamValues& params) const;

    // Total size of the encoder's output: one value per continuous dimension,
    // one per choice of each choice dimension.
    std::size_t outputSize() const;

    static const SearchSpace& standard();
};

// The gain used for every candidate render. The matcher works out the right
// gain afterwards by comparing levels, so it never has to search for it.
inline constexpr float kSearchGainDb = -12.0f;

// For a given preset, which dimensions can actually be heard. A silent
// oscillator's waveform, or the settings of an effect that is off, make no
// difference to the sound, so nothing should be learned from them.
std::vector<bool> audibleDims(const SearchSpace& space, const ParamValues& params);

} // namespace synthmatch::match
