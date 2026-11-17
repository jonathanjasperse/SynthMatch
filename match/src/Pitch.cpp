#include "synthmatch/match/Pitch.h"

#include "synthmatch/match/Features.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace synthmatch::match {

namespace {

constexpr double kMinHz = 27.5;   // A0
constexpr double kMaxHz = 4200.0; // just above C8
constexpr double kFrameSec = 0.010;
constexpr double kActiveRangeDb = 45.0; // quieter than this below the peak counts as silence
constexpr std::size_t kNewPitchFrames = 15; // 150 ms at a new pitch makes a new note
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

// Stacked oscillators (a sub-octave, a fifth above) make the pitch tracker
// flip between readings an octave or a fifth apart. Such a jump, without a
// new attack, is the same note.
bool harmonicallyRelated(double semitones)
{
    const double a = std::abs(semitones);
    // Octaves get more slack: vibrato moves both readings, but the tracker
    // can flip mid-swing.
    for (const double octave : {12.0, 24.0})
        if (std::abs(a - octave) < 1.0)
            return true;
    for (const double fifth : {5.0, 7.0, 17.0, 19.0})
        if (std::abs(a - fifth) < 0.5)
            return true;
    return false;
}

// The pitch heard most often, to the nearest semitone.
double mostCommonNote(const std::vector<double>& pitches)
{
    if (pitches.empty())
        return std::numeric_limits<double>::quiet_NaN();
    std::vector<int> counts(128, 0);
    for (const double p : pitches)
        counts[static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(p)), 0, 127))]++;
    const auto best = std::max_element(counts.begin(), counts.end());
    const int note = static_cast<int>(best - counts.begin());
    // Refine within that semitone, so slightly detuned notes stay detuned.
    double sum = 0.0;
    int n = 0;
    for (const double p : pitches)
        if (std::lround(p) == note) {
            sum += p;
            ++n;
        }
    return sum / n;
}

double median(std::vector<double> values)
{
    if (values.empty())
        return kNaN;
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

struct Tracked {
    std::vector<PitchFrame> frames;
    std::size_t hop = 1;       // in samples of the signal that was analysed
    std::size_t window = 1;
    std::size_t divider = 1;   // analysed rate = input rate / divider
};

Tracked track(std::span<const float> input, double sampleRate)
{
    Tracked out;

    // Pitch needs no treble, so halve the rate when we can: a quarter of the work.
    std::vector<float> reduced;
    std::span<const float> x = input;
    double rate = sampleRate;
    if (sampleRate >= 44100.0) {
        reduced = decimateBy2(input);
        x = reduced;
        rate = sampleRate / 2.0;
        out.divider = 2;
    }

    const auto tauMin = std::max<std::size_t>(2, static_cast<std::size_t>(rate / kMaxHz));
    const auto tauMax = static_cast<std::size_t>(rate / kMinHz);
    const std::size_t window = static_cast<std::size_t>(rate * 0.043); // samples compared at each lag
    const std::size_t hop = std::max<std::size_t>(1, static_cast<std::size_t>(rate * kFrameSec));
    out.hop = hop;
    out.window = window;

    if (x.size() < window)
        return out;

    // Pad with silence so the frames run right to the end of the recording.
    std::vector<float> padded(x.begin(), x.end());
    padded.resize(x.size() + window + tauMax, 0.0f);
    x = padded;

    std::vector<double> diff(tauMax + 1);
    const std::size_t numFrames = (x.size() - window - tauMax) / hop + 1;
    out.frames.resize(numFrames);

    for (std::size_t f = 0; f < numFrames; ++f) {
        const float* s = x.data() + f * hop;
        PitchFrame& frame = out.frames[f];
        frame.timeSec = (static_cast<double>(f * hop) + 0.5 * static_cast<double>(window)) / rate;

        double energy = 0.0;
        for (std::size_t j = 0; j < window; ++j)
            energy += static_cast<double>(s[j]) * s[j];
        frame.levelDb = 10.0 * std::log10(energy / static_cast<double>(window) + 1.0e-12);
        if (frame.levelDb < -100.0)
            continue;

        // YIN step 1: how different is the signal from itself shifted by tau?
        // It is least different when tau is one period.
        for (std::size_t tau = 1; tau <= tauMax; ++tau) {
            double d = 0.0;
            const float* shifted = s + tau;
            for (std::size_t j = 0; j < window; ++j) {
                const double delta = static_cast<double>(s[j]) - shifted[j];
                d += delta * delta;
            }
            diff[tau] = d;
        }

        // Step 2: divide by the running average, so small lags don't win by default.
        double running = 0.0;
        diff[0] = 1.0;
        for (std::size_t tau = 1; tau <= tauMax; ++tau) {
            running += diff[tau];
            diff[tau] = running > 0.0 ? diff[tau] * static_cast<double>(tau) / running : 1.0;
        }

        // Step 3: the first clear dip is the period; failing that, the deepest one.
        std::size_t best = 0;
        for (std::size_t tau = tauMin; tau < tauMax; ++tau) {
            if (diff[tau] < 0.15) {
                while (tau + 1 < tauMax && diff[tau + 1] < diff[tau])
                    ++tau;
                best = tau;
                break;
            }
        }
        if (best == 0) {
            best = tauMin;
            for (std::size_t tau = tauMin; tau < tauMax; ++tau)
                if (diff[tau] < diff[best])
                    best = tau;
        }

        // Step 4: a parabola through the dip and its neighbours finds the
        // period between samples.
        double period = static_cast<double>(best);
        if (best > tauMin && best + 1 < tauMax) {
            const double a = diff[best - 1], b = diff[best], c = diff[best + 1];
            const double denom = a - 2.0 * b + c;
            if (denom > 1.0e-12)
                period += 0.5 * (a - c) / denom;
        }

        frame.hz = rate / period;
        frame.confidence = std::clamp(1.0 - diff[best], 0.0, 1.0);
    }
    return out;
}

} // namespace

std::vector<PitchFrame> trackPitch(std::span<const float> mono, double sampleRate)
{
    return track(mono, sampleRate).frames;
}

NoteDetection detectNotes(std::span<const float> mono, double sampleRate, double maxSeconds)
{
    NoteDetection result;
    const Tracked tracked = track(mono, sampleRate);
    const auto& frames = tracked.frames;
    if (frames.empty())
        return result;

    double peakDb = -200.0;
    for (const auto& f : frames)
        peakDb = std::max(peakDb, f.levelDb);
    if (peakDb < -80.0)
        return result; // silence

    const std::size_t n = frames.size();
    std::vector<bool> active(n);
    std::vector<double> midi(n, kNaN);
    double confidenceSum = 0.0;
    std::size_t activeCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        active[i] = frames[i].levelDb > peakDb - kActiveRangeDb;
        if (active[i]) {
            confidenceSum += frames[i].confidence;
            ++activeCount;
            if (frames[i].confidence > 0.6 && frames[i].hz > 0.0)
                midi[i] = hzToMidi(frames[i].hz);
        }
    }
    result.confidence = activeCount > 0 ? confidenceSum / static_cast<double>(activeCount) : 0.0;

    // Smooth out single-frame pitch glitches.
    std::vector<double> smooth(n, kNaN);
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<double> near;
        for (std::size_t j = i >= 2 ? i - 2 : 0; j < std::min(n, i + 3); ++j)
            if (!std::isnan(midi[j]))
                near.push_back(midi[j]);
        if (near.size() >= 2)
            smooth[i] = median(near);
    }

    // Split into notes.
    struct Segment {
        std::size_t first = 0, last = 0; // frames, inclusive
        bool endsInSilence = true;       // false when the next note starts straight away
    };
    std::vector<Segment> segments;
    bool inNote = false;
    Segment current;
    std::vector<double> pitches; // of the current note so far
    std::size_t quiet = 0, drift = 0;
    double noteMaxDb = -200.0;   // loudest frame of the current note so far

    const auto close = [&](std::size_t last, bool endsInSilence) {
        current.last = last;
        current.endsInSilence = endsInSilence;
        if (current.last >= current.first + 2) // at least 30 ms
            segments.push_back(current);
        inNote = false;
    };

    for (std::size_t i = 0; i < n; ++i) {
        if (!inNote) {
            if (active[i]) {
                inNote = true;
                current.first = i;
                pitches.clear();
                quiet = drift = 0;
                noteMaxDb = -200.0;
            } else {
                continue;
            }
        }

        if (!active[i]) {
            if (++quiet >= 3)
                close(i - quiet, true);
            continue;
        }
        quiet = 0;

        if (!std::isnan(smooth[i])) {
            // A new note when the pitch moves away from the note's centre and
            // stays there, steady, for 150 ms (legato playing). Vibrato swings
            // around the centre and never holds still; an octave or a fifth is
            // the tracker hesitating between stacked oscillators.
            const double away = pitches.size() >= 4 ? smooth[i] - median(pitches) : 0.0;
            const bool related = std::abs(away) > 0.7 && harmonicallyRelated(away);
            if (!related)
                pitches.push_back(smooth[i]); // every frame, so vibrato centres itself
            bool steady = false;
            if (std::abs(away) > 0.7 && !related && drift + 1 >= kNewPitchFrames) {
                double lo = smooth[i], hi = smooth[i];
                for (std::size_t j = i + 1 - kNewPitchFrames; j <= i; ++j)
                    if (!std::isnan(smooth[j])) {
                        lo = std::min(lo, smooth[j]);
                        hi = std::max(hi, smooth[j]);
                    }
                steady = hi - lo < 0.8;
            }
            if (std::abs(away) > 0.7 && !related) {
                ++drift;
                if (drift >= kNewPitchFrames && steady) {
                    const std::size_t boundary = i + 1 - drift;
                    close(boundary > 0 ? boundary - 1 : 0, false);
                    inNote = true;
                    current.first = boundary;
                    pitches.clear();
                    for (std::size_t j = boundary; j <= i; ++j)
                        if (!std::isnan(smooth[j]))
                            pitches.push_back(smooth[j]);
                    drift = 0;
                    noteMaxDb = -200.0;
                }
            } else if (!related) {
                drift = 0;
            }
        }

        // A new note when the level jumps back up (the same pitch played again).
        if (i >= current.first + 8 && i >= 5) {
            double recentMin = frames[i].levelDb;
            for (std::size_t j = i - 5; j < i; ++j)
                recentMin = std::min(recentMin, frames[j].levelDb);
            // Only after a deep dip, and back up to nearly full level: unison
            // beating and tremolo wobble the level too, but not like that.
            double comingPeak = frames[i].levelDb;
            for (std::size_t j = i; j < std::min(n, i + 5); ++j)
                comingPeak = std::max(comingPeak, frames[j].levelDb);
            if (frames[i].levelDb - recentMin > 12.0 && frames[i].levelDb > peakDb - 20.0 &&
                noteMaxDb - recentMin > 15.0 && comingPeak > noteMaxDb - 6.0) {
                close(i - 1, true); // the key was released somewhere before the dip
                inNote = true;
                current.first = i;
                pitches.clear();
                drift = 0;
                noteMaxDb = -200.0;
            }
        }
        noteMaxDb = std::max(noteMaxDb, frames[i].levelDb);
    }
    if (inNote)
        close(n - 1, true);
    if (segments.empty())
        return result;

    const double rate = sampleRate / static_cast<double>(tracked.divider);
    const double frameStep = static_cast<double>(tracked.hop) / rate;
    const double halfWindow = 0.5 * static_cast<double>(tracked.window) / rate;
    const auto frameStartSec = [&](std::size_t frame) { return static_cast<double>(frame) * frameStep; };

    // The first note starts at the first sample that rises clearly above the noise.
    float peakAbs = 0.0f;
    for (const float v : mono)
        peakAbs = std::max(peakAbs, std::abs(v));
    const float onsetThreshold = 0.02f * peakAbs;
    auto firstSample = static_cast<std::size_t>(frameStartSec(segments.front().first) * sampleRate);
    while (firstSample < mono.size() && std::abs(mono[firstSample]) < onsetThreshold)
        ++firstSample;
    const double originSec = static_cast<double>(firstSample) / sampleRate;

    double loudest = -200.0;
    struct Draft {
        double startSec, endSec, midi, levelDb;
    };
    std::vector<Draft> drafts;
    for (std::size_t s = 0; s < segments.size(); ++s) {
        const auto& seg = segments[s];

        std::vector<double> notePitches, levels;
        double level = -200.0;
        for (std::size_t i = seg.first; i <= seg.last; ++i) {
            if (!std::isnan(smooth[i]))
                notePitches.push_back(smooth[i]);
            levels.push_back(frames[i].levelDb);
            level = std::max(level, frames[i].levelDb);
        }

        Draft d;
        d.midi = mostCommonNote(notePitches);
        d.levelDb = level;
        if (s == 0) {
            d.startSec = originSec;
        } else if (segments[s - 1].endsInSilence) {
            // After a gap: the first sample that rises above the noise, as for the first note.
            auto sample = static_cast<std::size_t>(frameStartSec(seg.first) * sampleRate);
            const auto limit = std::min(mono.size(), sample + static_cast<std::size_t>(2.0 * halfWindow * sampleRate));
            while (sample < limit && std::abs(mono[sample]) < onsetThreshold)
                ++sample;
            d.startSec = static_cast<double>(sample) / sampleRate;
        } else {
            d.startSec = frameStartSec(seg.first) + halfWindow; // a change of pitch: the middle of the frame
        }

        // Key-up shows as the level falling much faster than it was before.
        // (With a long release, or a pluck that has died away, it can't be
        // seen, and the estimate below is used instead.)
        std::size_t keyUp = 0;
        if (seg.endsInSilence) {
            const std::size_t count = levels.size();
            std::size_t top = 0;
            for (std::size_t k = 0; k < count; ++k)
                if (levels[k] > levels[top])
                    top = k;
            constexpr std::size_t span = 3; // 30 ms
            double steepest = 0.0;
            std::size_t at = 0;
            // Frames running past the end of the recording fall to silence
            // because the recording stops, not because a key was released.
            const std::size_t realFrames =
                std::min(frames.size(), static_cast<std::size_t>(static_cast<double>(mono.size()) / sampleRate / frameStep));
            const std::size_t usable = std::min(count, realFrames > seg.first + 6 ? realFrames - seg.first - 6 : 0);
            for (std::size_t k = top + 2; k + span < usable; ++k) {
                const double fall = levels[k] - levels[k + span];
                if (fall > steepest) {
                    steepest = fall;
                    at = k;
                }
            }
            if (at > top + 2 && steepest > 6.0) {
                // How fast it was falling before (its decay towards sustain).
                const double before = (levels[top] - levels[at]) / static_cast<double>(at - top) * span;
                if (steepest > 2.5 * std::max(before, 0.5))
                    keyUp = seg.first + at;
            }
        }

        if (keyUp != 0) {
            d.endSec = frameStartSec(keyUp) + halfWindow;
        } else if (seg.endsInSilence) {
            // The key was released where the level last sat near its sustained
            // value; after that it is the release tail.
            const std::size_t count = levels.size();
            std::vector<double> middle(levels.begin() + static_cast<std::ptrdiff_t>(count / 4),
                                       levels.begin() + static_cast<std::ptrdiff_t>(std::max(count / 4 + 1, 3 * count / 4)));
            const double sustain = median(middle);
            std::size_t lastSustained = seg.first;
            for (std::size_t i = seg.first; i <= seg.last; ++i)
                if (frames[i].levelDb >= sustain - 6.0)
                    lastSustained = i;
            d.endSec = frameStartSec(lastSustained) + halfWindow;
        } else {
            d.endSec = frameStartSec(seg.last + 1) + halfWindow;
        }
        d.endSec = std::max(d.endSec, d.startSec + 0.03);

        loudest = std::max(loudest, level);
        drafts.push_back(d);
    }

    // While one note hands over to the next, the tracker can briefly report a
    // pitch that belongs to neither. Fold such slivers into the note that follows.
    for (std::size_t i = 0; i + 1 < drafts.size();) {
        const bool sliver = drafts[i].endSec - drafts[i].startSec < 0.06;
        const bool touching = drafts[i + 1].startSec - drafts[i].endSec < 0.03;
        if (sliver && touching) {
            drafts[i + 1].startSec = drafts[i].startSec;
            drafts.erase(drafts.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // Notes with no clear pitch borrow the typical pitch of the others.
    std::vector<double> known;
    for (const auto& d : drafts)
        if (!std::isnan(d.midi))
            known.push_back(d.midi);
    const double fallback = known.empty() ? 60.0 : median(known);

    const double limitSec = originSec + maxSeconds;
    double endSec = originSec;
    for (const auto& d : drafts) {
        if (d.startSec >= limitSec || result.notes.size() >= 32)
            break;
        NoteEvent note;
        note.startSec = std::max(0.0, d.startSec - originSec);
        note.durationSec = std::min(d.endSec, limitSec) - d.startSec;
        note.note = std::clamp(static_cast<int>(std::lround(std::isnan(d.midi) ? fallback : d.midi)), 0, 127);
        note.velocity = static_cast<float>(std::clamp(std::pow(10.0, (d.levelDb - loudest) / 20.0), 0.25, 1.0));
        if (note.durationSec < 0.02)
            continue;
        result.notes.push_back(note);
    }
    if (result.notes.empty())
        return result;

    // The analysed section runs to where the sound dies away (or the limit).
    std::size_t lastActive = segments.front().first;
    for (std::size_t i = 0; i < n; ++i)
        if (active[i] && frames[i].timeSec < limitSec)
            lastActive = i;
    endSec = std::min(limitSec, frameStartSec(lastActive) + 2.0 * halfWindow + 0.05);
    endSec = std::min(endSec, static_cast<double>(mono.size()) / sampleRate);

    result.startSample = firstSample;
    result.numSamples = static_cast<std::size_t>(std::max(0.0, endSec - originSec) * sampleRate);

    // Keep the track over the section, for measuring vibrato (Modulation.h).
    for (const auto& f : frames) {
        if (f.timeSec < originSec - 0.05 || f.timeSec > endSec + 0.05)
            continue;
        result.pitch.push_back(f);
        result.pitch.back().timeSec -= originSec;
    }
    return result;
}

} // namespace synthmatch::match
