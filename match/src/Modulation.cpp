#include "synthmatch/match/Modulation.h"

#include "synthmatch/dsp/Lfo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace synthmatch::match {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kSkipAttackSec = 0.15; // the start of a note is the attack, not the wobble
constexpr double kSkipEndSec = 0.02;
constexpr std::size_t kMinFrames = 40;
constexpr double kYinWindowSec = 0.043; // Pitch.cpp's window, which smooths fast vibrato a little

struct SineFit {
    double rateHz = 0.0;
    double amplitude = 0.0;
    double strength = 0.0;
    double cycles = 0.0;
    bool ok = false;
};

// Least squares for offset + slope * t (+ a cos + b sin at `rateHz` when it is
// above 0). Returns the coefficients; `residual` gets the mean squared error.
std::array<double, 4> leastSquares(std::span<const double> t, std::span<const double> x, double rateHz, double& residual)
{
    const std::size_t n = t.size();
    const std::size_t terms = rateHz > 0.0 ? 4 : 2;
    const auto basis = [&](std::size_t i, std::size_t k) {
        switch (k) {
        case 0: return 1.0;
        case 1: return t[i];
        case 2: return std::cos(kTwoPi * rateHz * t[i]);
        default: return std::sin(kTwoPi * rateHz * t[i]);
        }
    };
    std::array<std::array<double, 5>, 4> m{};
    for (std::size_t i = 0; i < n; ++i) {
        std::array<double, 4> row{};
        for (std::size_t k = 0; k < terms; ++k)
            row[k] = basis(i, k);
        for (std::size_t r = 0; r < terms; ++r) {
            for (std::size_t c = 0; c < terms; ++c)
                m[r][c] += row[r] * row[c];
            m[r][4] += row[r] * x[i];
        }
    }
    // Gaussian elimination with partial pivoting.
    std::array<double, 4> coef{};
    for (std::size_t c = 0; c < terms; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < terms; ++r)
            if (std::abs(m[r][c]) > std::abs(m[pivot][c]))
                pivot = r;
        std::swap(m[c], m[pivot]);
        if (std::abs(m[c][c]) < 1.0e-12) {
            residual = 0.0;
            return coef;
        }
        for (std::size_t r = 0; r < terms; ++r) {
            if (r == c)
                continue;
            const double f = m[r][c] / m[c][c];
            for (std::size_t k = c; k < 5; ++k)
                m[r][k] -= f * m[c][k];
        }
    }
    for (std::size_t k = 0; k < terms; ++k)
        coef[k] = m[k][4] / m[k][k];

    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double fit = 0.0;
        for (std::size_t k = 0; k < terms; ++k)
            fit += coef[k] * basis(i, k);
        sum += (x[i] - fit) * (x[i] - fit);
    }
    residual = sum / static_cast<double>(n);
    return coef;
}

// The steadiest sine wave in a series (times in seconds, not necessarily
// evenly spaced), between loHz and hiHz. A slow drift is allowed for.
SineFit fitSine(std::vector<double> t, std::span<const double> x, double loHz, double hiHz)
{
    SineFit fit;
    const std::size_t n = t.size();
    if (n < kMinFrames)
        return fit;
    const double t0 = t.front(), span = t.back() - t.front();
    if (span <= 0.0)
        return fit;
    for (auto& v : t)
        v -= t0;

    double driftResidual = 0.0;
    const auto drift = leastSquares(t, x, 0.0, driftResidual);
    if (driftResidual <= 1.0e-12)
        return fit; // nothing moves

    // Scan the rates with a Hann window, stepping each frame's phasor by a
    // fixed rotation per step.
    constexpr double step = 0.02;
    const auto steps = static_cast<std::size_t>(std::floor((hiHz - loHz) / step)) + 1;
    std::vector<std::complex<double>> phasor(n), rotate(n);
    std::vector<double> weighted(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * t[i] / span);
        weighted[i] = w * (x[i] - drift[0] - drift[1] * t[i]);
        phasor[i] = std::polar(1.0, -kTwoPi * loHz * t[i]);
        rotate[i] = std::polar(1.0, -kTwoPi * step * t[i]);
    }
    std::vector<double> magnitude(steps);
    for (std::size_t s = 0; s < steps; ++s) {
        std::complex<double> sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += weighted[i] * phasor[i];
            phasor[i] *= rotate[i];
        }
        magnitude[s] = std::abs(sum);
    }
    const auto peak = static_cast<std::size_t>(std::max_element(magnitude.begin(), magnitude.end()) - magnitude.begin());
    double where = static_cast<double>(peak);
    if (peak > 0 && peak + 1 < steps) {
        const double a = magnitude[peak - 1], b = magnitude[peak], c = magnitude[peak + 1];
        const double denom = a - 2.0 * b + c;
        if (denom < 0.0)
            where += std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
    }
    fit.rateHz = loHz + where * step;

    double residual = 0.0;
    const auto sine = leastSquares(t, x, fit.rateHz, residual);
    fit.amplitude = std::hypot(sine[2], sine[3]);
    fit.strength = std::clamp(1.0 - residual / driftResidual, 0.0, 1.0);
    fit.cycles = fit.rateHz * span;
    fit.ok = true;
    return fit;
}

// The amplitude of the sine wave at exactly `rateHz` in a series.
double amplitudeAt(std::vector<double> t, std::span<const double> x, double rateHz)
{
    if (t.size() < kMinFrames || rateHz <= 0.0)
        return 0.0;
    const double t0 = t.front();
    for (auto& v : t)
        v -= t0;
    double residual = 0.0;
    const auto sine = leastSquares(t, x, rateHz, residual);
    return std::hypot(sine[2], sine[3]);
}

struct NoteFit {
    SineFit fit;
    double value = 0.0;  // the amplitude in the estimate's units
    double extra = 0.0;  // tremolo: the brightness swing
    double weight = 0.0; // seconds measured
};

double weightedMedian(std::vector<std::pair<double, double>> valueWeight)
{
    if (valueWeight.empty())
        return 0.0;
    std::sort(valueWeight.begin(), valueWeight.end());
    double total = 0.0;
    for (const auto& [v, w] : valueWeight)
        total += w;
    double running = 0.0;
    for (const auto& [v, w] : valueWeight) {
        running += w;
        if (running >= 0.5 * total)
            return v;
    }
    return valueWeight.back().first;
}

// Pools the notes: the weighted median rate and amplitude, the weighted mean
// strength, and the most cycles seen in one note.
void pool(const std::vector<NoteFit>& fits, double& rate, double& amplitude, double& strength, double& cycles, double* extra)
{
    std::vector<std::pair<double, double>> rates, amplitudes;
    double weightSum = 0.0, strengthSum = 0.0, extraSum = 0.0;
    for (const auto& f : fits) {
        rates.push_back({f.fit.rateHz, f.weight});
        amplitudes.push_back({f.value, f.weight});
        weightSum += f.weight;
        strengthSum += f.weight * f.fit.strength;
        extraSum += f.weight * f.extra;
        cycles = std::max(cycles, f.fit.cycles);
    }
    if (weightSum <= 0.0)
        return;
    rate = weightedMedian(rates);
    amplitude = weightedMedian(amplitudes);
    strength = strengthSum / weightSum;
    if (extra)
        *extra = extraSum / weightSum;
}

double sinc(double x) { return std::abs(x) < 1.0e-9 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x); }

// The rates the modulation term listens at: 16, evenly spaced on a log scale.
constexpr std::size_t kRates = 16;
double modulationRate(std::size_t k) { return 1.5 * std::pow(16.0 / 1.5, static_cast<double>(k) / (kRates - 1)); }
constexpr std::size_t kBandsPerGroup = 4;

} // namespace

ModulationEstimate measureModulation(std::span<const PitchFrame> pitch, const Spectrogram& spectrogram,
                                     const SpectrogramAnalyzer& analyzer, std::span<const NoteEvent> notes)
{
    ModulationEstimate out;

    // Vibrato: the pitch track of each held note, in cents from its middle.
    std::vector<NoteFit> vibrato;
    for (const auto& note : notes) {
        const double from = note.startSec + kSkipAttackSec, to = note.startSec + note.durationSec - kSkipEndSec;
        std::vector<double> t, hz;
        for (const auto& f : pitch) {
            if (f.timeSec >= from && f.timeSec <= to && f.confidence > 0.6 && f.hz > 0.0) {
                t.push_back(f.timeSec);
                hz.push_back(f.hz);
            }
        }
        if (t.size() < kMinFrames)
            continue;
        std::vector<double> sorted = hz;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        const double centre = sorted[sorted.size() / 2];
        std::vector<double> times, cents;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double c = 1200.0 * std::log2(hz[i] / centre);
            if (std::abs(c) <= 150.0) { // not an octave or fifth slip
                times.push_back(t[i]);
                cents.push_back(c);
            }
        }
        NoteFit nf;
        nf.fit = fitSine(times, cents, 1.5, 12.0);
        if (!nf.fit.ok)
            continue;
        // The pitch tracker averages over its window, which shrinks fast vibrato.
        nf.value = nf.fit.amplitude / std::max(0.3, sinc(nf.fit.rateHz * kYinWindowSec));
        nf.weight = times.back() - times.front();
        vibrato.push_back(nf);
    }
    auto& v = out.vibrato;
    pool(vibrato, v.rateHz, v.cents, v.strength, v.cycles, nullptr);
    v.found = v.rateHz >= 2.0 && v.rateHz <= 12.0 && v.cents >= 4.0 && v.strength >= 0.7 && v.cycles >= 2.0;

    // Tremolo: the level of each held note. The brightness is checked too:
    // detuned oscillators and a moving cutoff wobble the level as well, but
    // they move the brightness much more than a tremolo does.
    const auto& config = analyzer.config();
    const double hopSec = static_cast<double>(config.hop) / analyzer.sampleRate();
    const double halfWindow = 0.5 * static_cast<double>(config.fftSize) / analyzer.sampleRate();
    std::vector<NoteFit> tremolo;
    for (const auto& note : notes) {
        const double from = note.startSec + kSkipAttackSec, to = note.startSec + note.durationSec - kSkipEndSec;
        std::vector<double> t, level, brightness;
        for (std::size_t f = 0; f < spectrogram.numFrames; ++f) {
            const double time = static_cast<double>(f) * hopSec + halfWindow;
            if (time < from || time > to)
                continue;
            double weighted = 0.0, total = 0.0;
            for (std::size_t b = 0; b < spectrogram.numBands; ++b) {
                const double p = std::pow(10.0, spectrogram.at(f, b) / 10.0);
                weighted += p * analyzer.bandHz(b);
                total += p;
            }
            if (total <= 0.0)
                continue;
            t.push_back(time);
            level.push_back(spectrogram.frameDb[f]);
            brightness.push_back(1200.0 * std::log2(weighted / total));
        }
        NoteFit nf;
        nf.fit = fitSine(t, level, 1.5, 15.0);
        if (!nf.fit.ok)
            continue;
        nf.value = nf.fit.amplitude;
        nf.extra = amplitudeAt(t, brightness, nf.fit.rateHz);
        nf.weight = t.back() - t.front();
        tremolo.push_back(nf);
    }
    auto& tr = out.tremolo;
    pool(tremolo, tr.rateHz, tr.db, tr.strength, tr.cycles, &tr.brightnessCents);
    tr.found = !v.found && tr.rateHz >= 1.5 && tr.rateHz <= 15.0 && tr.db >= 0.4 && tr.strength >= 0.8 &&
               tr.brightnessCents <= 60.0 && tr.cycles >= 2.0;
    return out;
}

double vibratoDepth(double cents)
{
    // The engine's pitch LFO swings +/- kLfoPitchSemitones * depth^2.
    return std::clamp(std::sqrt(std::max(0.0, cents) / (100.0 * dsp::kLfoPitchSemitones)), 0.0, 1.0);
}

double tremoloDepth(double db)
{
    // The engine's amp LFO swings the level between 1 and 1 - depth.
    return std::clamp(1.0 - std::pow(10.0, -std::max(0.0, db) / 10.0), 0.0, 1.0);
}

ModulationProfile::ModulationProfile(const ModulationEstimate& estimate, const Spectrogram& reference,
                                     const SpectrogramAnalyzer& analyzer, std::span<const NoteEvent> notes)
{
    const double rate = estimate.rateHz();
    if (rate <= 0.0 || reference.numFrames == 0 || reference.numBands < kBandsPerGroup)
        return;
    const auto& config = analyzer.config();
    const double hopSec = static_cast<double>(config.hop) / analyzer.sampleRate();
    const double halfWindow = 0.5 * static_cast<double>(config.fftSize) / analyzer.sampleRate();
    period_ = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(1.0 / (rate * hopSec))));
    groups_ = reference.numBands / kBandsPerGroup;
    floor_ = reference.peakDb - 70.0f;

    // The held part of each note, at least two periods long.
    std::size_t taken = 0;
    for (const auto& note : notes) {
        const double from = (note.startSec + kSkipAttackSec - halfWindow) / hopSec;
        const double to = (note.startSec + note.durationSec - kSkipEndSec - halfWindow) / hopSec;
        if (to < 0.0)
            continue;
        const auto first = std::max(taken, static_cast<std::size_t>(std::ceil(std::max(0.0, from))));
        const auto last = std::min(reference.numFrames - 1, static_cast<std::size_t>(std::floor(to)));
        if (last < first || last + 1 - first < 2 * period_)
            continue;
        Run run;
        run.first = first;
        run.count = last + 1 - first;
        run.twiddles.resize(kRates * run.count);
        for (std::size_t i = 0; i < run.count; ++i) {
            const double w = 0.5 - 0.5 * std::cos(kTwoPi * (static_cast<double>(i) + 0.5) / static_cast<double>(run.count));
            run.windowSum += w;
            for (std::size_t k = 0; k < kRates; ++k)
                run.twiddles[k * run.count + i] = std::polar(w, -kTwoPi * modulationRate(k) * static_cast<double>(i) * hopSec);
        }
        runs_.push_back(std::move(run));
        taken = last + 1;
    }
    if (runs_.empty())
        return;

    // Regions that are loud in the reference count most.
    groupWeight_.assign(groups_, 0.0);
    std::size_t frames = 0;
    for (const auto& run : runs_) {
        for (std::size_t i = 0; i < run.count; ++i)
            for (std::size_t g = 0; g < groups_; ++g)
                for (std::size_t b = g * kBandsPerGroup; b < (g + 1) * kBandsPerGroup; ++b)
                    groupWeight_[g] += std::max(reference.at(run.first + i, b), floor_);
        frames += run.count;
    }
    double weightSum = 0.0;
    for (auto& w : groupWeight_) {
        const double mean = w / static_cast<double>(frames * kBandsPerGroup);
        w = std::clamp((mean - floor_) / 70.0, 0.0, 1.0);
        weightSum += w;
    }
    if (weightSum <= 0.0) {
        runs_.clear();
        return;
    }
    referenceDepth_ = depths(reference, 0.0f);
}

std::vector<double> ModulationProfile::depths(const Spectrogram& s, float gainDb) const
{
    std::vector<double> out(groups_ * kRates, 0.0);
    std::size_t total = 0;
    for (const auto& run : runs_)
        total += run.count;
    std::vector<double> y;
    for (const auto& run : runs_) {
        const double share = static_cast<double>(run.count) / static_cast<double>(total);
        y.resize(run.count);
        for (std::size_t g = 0; g < groups_; ++g) {
            // The region's level over the run, with its straight-line trend removed.
            double sumT = 0.0, sumY = 0.0, sumTT = 0.0, sumTY = 0.0;
            for (std::size_t i = 0; i < run.count; ++i) {
                const std::size_t f = run.first + i;
                double level = 0.0;
                for (std::size_t b = g * kBandsPerGroup; b < (g + 1) * kBandsPerGroup; ++b)
                    level += f < s.numFrames ? std::max(s.at(f, b) + gainDb, floor_) : floor_;
                y[i] = level / kBandsPerGroup;
                const double t = static_cast<double>(i);
                sumT += t;
                sumY += y[i];
                sumTT += t * t;
                sumTY += t * y[i];
            }
            const double n = static_cast<double>(run.count);
            const double slope = (n * sumTY - sumT * sumY) / (n * sumTT - sumT * sumT);
            const double offset = (sumY - slope * sumT) / n;
            for (std::size_t i = 0; i < run.count; ++i)
                y[i] -= offset + slope * static_cast<double>(i);

            for (std::size_t k = 0; k < kRates; ++k) {
                const auto* tw = run.twiddles.data() + k * run.count;
                std::complex<double> sum = 0.0;
                for (std::size_t i = 0; i < run.count; ++i)
                    sum += y[i] * tw[i];
                out[g * kRates + k] += share * 2.0 * std::abs(sum) / run.windowSum;
            }
        }
    }
    return out;
}

Spectrogram ModulationProfile::smooth(const Spectrogram& s) const
{
    Spectrogram out = s;
    if (!active())
        return out;
    std::vector<double> sums;
    for (const auto& run : runs_) {
        const std::size_t end = std::min(run.first + run.count, s.numFrames);
        if (end <= run.first)
            continue;
        const std::size_t count = end - run.first;
        // Running sums along time for each band (and the frame level, last),
        // so each frame's average over one period costs the same.
        const std::size_t lanes = s.numBands + 1;
        sums.assign((count + 1) * lanes, 0.0);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t f = run.first + i;
            for (std::size_t b = 0; b < s.numBands; ++b)
                sums[(i + 1) * lanes + b] = sums[i * lanes + b] + s.at(f, b);
            sums[(i + 1) * lanes + s.numBands] = sums[i * lanes + s.numBands] + s.frameDb[f];
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t lo = i >= period_ / 2 ? i - period_ / 2 : 0;
            const std::size_t hi = std::min(count, lo + period_);
            const double n = static_cast<double>(hi - lo);
            const std::size_t f = run.first + i;
            for (std::size_t b = 0; b < s.numBands; ++b)
                out.db[f * s.numBands + b] = static_cast<float>((sums[hi * lanes + b] - sums[lo * lanes + b]) / n);
            out.frameDb[f] = static_cast<float>((sums[hi * lanes + s.numBands] - sums[lo * lanes + s.numBands]) / n);
        }
    }
    return out;
}

double ModulationProfile::distance(const Spectrogram& candidate, float gainDb) const
{
    if (!active())
        return 0.0;
    const auto depth = depths(candidate, gainDb);
    double sum = 0.0, weights = 0.0;
    for (std::size_t g = 0; g < groups_; ++g) {
        double diff = 0.0;
        for (std::size_t k = 0; k < kRates; ++k)
            diff += std::abs(referenceDepth_[g * kRates + k] - depth[g * kRates + k]);
        sum += groupWeight_[g] * diff;
        weights += groupWeight_[g];
    }
    return weights > 0.0 ? weight_ * sum / (4.0 * weights) : 0.0;
}

} // namespace synthmatch::match
