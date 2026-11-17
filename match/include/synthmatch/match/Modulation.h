#pragma once

// Vibrato and tremolo: measuring them in a recording, so the search can start
// with the right LFO, and comparing them between two sounds without caring
// about the LFO's phase. The engine restarts its LFOs on every note; in a
// recording the wobble rarely starts at the same point, so a frame-by-frame
// comparison would score the right vibrato worse than none at all.

#include "synthmatch/Render.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Pitch.h"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace synthmatch::match {

// The raw measurements are always filled in (the encoder learns from them);
// `found` says whether they are clear enough to act on.
struct VibratoEstimate {
    double rateHz = 0.0;
    double cents = 0.0;    // swing either side of the centre pitch
    double strength = 0.0; // 0..1: how much of the pitch movement a steady sine wave explains
    double cycles = 0.0;   // in the longest note measured
    bool found = false;
};

struct TremoloEstimate {
    double rateHz = 0.0;
    double db = 0.0;              // level swing either side of the centre
    double strength = 0.0;
    double brightnessCents = 0.0; // how far the brightness swings at the same rate
    double cycles = 0.0;
    bool found = false;           // only when there is no vibrato
};

struct ModulationEstimate {
    VibratoEstimate vibrato;
    TremoloEstimate tremolo;

    bool found() const { return vibrato.found || tremolo.found; }
    double rateHz() const { return vibrato.found ? vibrato.rateHz : (tremolo.found ? tremolo.rateHz : 0.0); }
};

// Measures vibrato from the pitch track and tremolo from the spectrogram's
// level, over the held part of each note. `pitch` and `notes` use the same
// time origin as the spectrogram (its first frame starts at 0). Deterministic.
ModulationEstimate measureModulation(std::span<const PitchFrame> pitch, const Spectrogram& spectrogram,
                                     const SpectrogramAnalyzer& analyzer, std::span<const NoteEvent> notes);

// LFO depths, in the engine's units, for a vibrato of +/- `cents` (pitch
// target) and for a tremolo of +/- `db` (amp target).
double vibratoDepth(double cents);
double tremoloDepth(double db);

// How the distance treats a modulated reference. Inactive (the default, and
// whenever no vibrato or tremolo was found) means the plain spectral distance,
// exactly as for any steady sound.
//
// When active, both spectrograms are averaged over one measured period while
// the notes are held, which removes the LFO's phase, and a second term
// compares how strongly each frequency region wobbles at a range of rates.
class ModulationProfile {
public:
    ModulationProfile() = default;
    ModulationProfile(const ModulationEstimate& estimate, const Spectrogram& reference,
                      const SpectrogramAnalyzer& analyzer, std::span<const NoteEvent> notes);

    bool active() const { return !runs_.empty(); }

    // A copy with the held frames averaged over one period.
    Spectrogram smooth(const Spectrogram& spectrogram) const;

    // The phase-free difference in wobble between the reference and a
    // candidate (raw, not smoothed), in dB. `gainDb` is added to the candidate.
    double distance(const Spectrogram& candidate, float gainDb) const;

private:
    struct Run {
        std::size_t first = 0, count = 0;            // frames
        std::vector<std::complex<double>> twiddles; // rates x frames, window included
        double windowSum = 0.0;
    };

    std::vector<double> depths(const Spectrogram& spectrogram, float gainDb) const; // groups x rates

    std::vector<Run> runs_;
    std::size_t period_ = 0; // frames
    std::size_t groups_ = 0;
    float floor_ = -200.0f;
    std::vector<double> groupWeight_;
    std::vector<double> referenceDepth_;
    double weight_ = 1.0;
};

} // namespace synthmatch::match
