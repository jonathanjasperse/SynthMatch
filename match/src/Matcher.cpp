#include "synthmatch/match/Matcher.h"

#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Modulation.h"
#include "synthmatch/match/Optimizer.h"
#include "synthmatch/match/Parallel.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/SearchSpace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace synthmatch::match {

namespace {

// How much closeness the clean-up after the search may give up for a simpler preset.
constexpr double kSimplifyMarginDb = 0.15;

SpectrogramConfig configFor(double sampleRate)
{
    SpectrogramConfig c;
    c.fftSize = sampleRate <= 26000.0 ? 1024 : 2048; // about 43 ms either way
    c.hop = c.fftSize / 4;
    return c;
}

// Scores candidate presets against the reference at one sample rate.
class Objective {
public:
    struct Score {
        double distance = 1.0e3;
        float gainDb = 0.0f; // the level change that lines the candidate up with the reference
    };

    // `oversample`: render candidates as the plugin plays them. The search
    // turns it off: twice as fast, at the cost of a little aliasing on high notes.
    Objective(std::vector<float> reference, double sampleRate, std::vector<NoteEvent> notes, bool oversample = true)
        : audio_(std::move(reference)), sampleRate_(sampleRate), notes_(std::move(notes)),
          analyzer_(sampleRate, configFor(sampleRate)), reference_(analyzer_.analyze(audio_))
    {
        double lastOff = 0.0;
        for (const auto& n : notes_)
            lastOff = std::max(lastOff, n.startSec + n.durationSec);
        settings_.sampleRate = sampleRate_;
        settings_.oversample = oversample;
        // Render exactly as much as the reference holds.
        settings_.tailSec = std::max(0.0, static_cast<double>(audio_.size()) / sampleRate_ - lastOff);
    }

    // When the reference has a measured vibrato or tremolo, candidates are
    // compared in a way that doesn't depend on the LFO's phase (Modulation.h).
    // Without one, the distance is the plain spectral distance.
    void useModulation(const ModulationEstimate& estimate)
    {
        profile_ = ModulationProfile(estimate, reference_, analyzer_, notes_);
        if (profile_.active())
            smoothedReference_ = profile_.smooth(reference_);
    }

    std::vector<float> renderMono(const ParamValues& params, const MatchEqGains* eq = nullptr) const
    {
        Preset preset;
        preset.params = params;
        if (eq)
            preset.matchEq = *eq;
        const auto audio = render(preset, notes_, settings_);
        return toMono(audio.left, audio.right);
    }

    // fitGain = false scores the preset at the level it actually plays at.
    Score evaluate(const ParamValues& params, const MatchEqGains* eq = nullptr, bool fitGain = true) const
    {
        const auto spectrogram = analyzer_.analyze(renderMono(params, eq));
        Score score;
        if (fitGain) {
            // Keep the final gain parameter inside its range.
            const auto& gain = spec(param::GainDb);
            const float current = params[param::GainDb];
            score.gainDb = std::clamp(bestGainDb(reference_, spectrogram), gain.min - current, gain.max - current);
        }
        if (profile_.active())
            score.distance = spectralDistance(smoothedReference_, profile_.smooth(spectrogram), score.gainDb) +
                             profile_.distance(spectrogram, score.gainDb);
        else
            score.distance = spectralDistance(reference_, spectrogram, score.gainDb);
        return score;
    }

    const std::vector<float>& audio() const { return audio_; }
    double durationSec() const { return static_cast<double>(audio_.size()) / sampleRate_; }
    const Spectrogram& reference() const { return reference_; }
    const SpectrogramAnalyzer& analyzer() const { return analyzer_; }
    double sampleRate() const { return sampleRate_; }
    const std::vector<NoteEvent>& notes() const { return notes_; }

private:
    std::vector<float> audio_;
    double sampleRate_;
    std::vector<NoteEvent> notes_;
    SpectrogramAnalyzer analyzer_;
    Spectrogram reference_;
    RenderSettings settings_;
    ModulationProfile profile_;
    Spectrogram smoothedReference_;
};

// Rough envelope, brightness and modulation measurements, to start the search
// somewhere sensible.
void applyMeasuredGuess(const Objective& objective, const ModulationEstimate& modulation, ParamValues& params)
{
    const auto set = [&](int index, double value) {
        params[static_cast<std::size_t>(index)] = clampToRange(spec(index), static_cast<float>(value));
    };
    if (modulation.vibrato.found) {
        set(param::Lfo1Target, static_cast<double>(dsp::LfoTarget::Pitch));
        set(param::Lfo1Wave, static_cast<double>(dsp::LfoWave::Sine));
        set(param::Lfo1RateHz, modulation.vibrato.rateHz);
        set(param::Lfo1Depth, vibratoDepth(modulation.vibrato.cents));
    } else if (modulation.tremolo.found) {
        set(param::Lfo1Target, static_cast<double>(dsp::LfoTarget::Amp));
        set(param::Lfo1Wave, static_cast<double>(dsp::LfoWave::Sine));
        set(param::Lfo1RateHz, modulation.tremolo.rateHz);
        set(param::Lfo1Depth, tremoloDepth(modulation.tremolo.db));
    }

    const auto& ref = objective.reference();
    const auto& config = objective.analyzer().config();
    const double hopSec = static_cast<double>(config.hop) / objective.sampleRate();
    const double halfWindow = 0.5 * static_cast<double>(config.fftSize) / objective.sampleRate();
    if (ref.numFrames < 4 || objective.notes().empty())
        return;

    const NoteEvent& note = objective.notes().front();
    const auto frameAt = [&](double sec) {
        const double f = (sec - halfWindow) / hopSec;
        return static_cast<std::size_t>(std::clamp(f, 0.0, static_cast<double>(ref.numFrames - 1)));
    };
    const auto timeOf = [&](std::size_t frame) { return static_cast<double>(frame) * hopSec + halfWindow; };

    const std::size_t first = frameAt(note.startSec);
    const std::size_t last = std::max(first, frameAt(note.startSec + note.durationSec - halfWindow));
    std::size_t peakFrame = first;
    for (std::size_t f = first; f <= last; ++f)
        if (ref.frameDb[f] > ref.frameDb[peakFrame])
            peakFrame = f;
    const float peakDb = ref.frameDb[peakFrame];
    const float endDb = ref.frameDb[last];

    // Attack: how long until the level is within 3 dB of its peak. The
    // analysis window blurs time by about 15 ms, so take that off.
    std::size_t risen = first;
    while (risen < peakFrame && ref.frameDb[risen] < peakDb - 3.0f)
        ++risen;
    const double attackMs = (timeOf(risen) - note.startSec) * 1000.0 - 15.0;

    const double sustain = std::clamp(std::pow(10.0, (endDb - peakDb) / 20.0), 0.0, 1.0);

    // Decay: the engine's decay time is the time to cover 99.9% of the way to
    // the sustain level, which is about ten times the time to cover half of it.
    double decayMs = 300.0;
    if (sustain < 0.85) {
        const double halfway = 20.0 * std::log10(0.5 * (1.0 + sustain));
        std::size_t half = peakFrame;
        while (half < last && ref.frameDb[half] > peakDb + halfway)
            ++half;
        decayMs = 10.0 * (timeOf(half) - timeOf(peakFrame)) * 1000.0;
    }

    // Release: three times the time to fall 20 dB after the key is let go.
    double releaseMs = 300.0;
    const std::size_t off = frameAt(note.startSec + note.durationSec + halfWindow);
    for (std::size_t f = off; f < ref.numFrames; ++f) {
        if (ref.frameDb[f] <= endDb - 20.0f) {
            releaseMs = 3.0 * (timeOf(f) - (note.startSec + note.durationSec)) * 1000.0 - 30.0;
            break;
        }
    }

    // Brightness: the frequency below which 98% of the held note's energy lies.
    std::vector<double> bandPower(ref.numBands, 0.0);
    double total = 0.0;
    for (std::size_t f = first; f <= last; ++f) {
        for (std::size_t b = 0; b < ref.numBands; ++b) {
            const double p = std::pow(10.0, ref.at(f, b) / 10.0);
            bandPower[b] += p;
            total += p;
        }
    }
    double cutoffHz = 20000.0, running = 0.0;
    for (std::size_t b = 0; b < ref.numBands && total > 0.0; ++b) {
        running += bandPower[b];
        if (running >= 0.98 * total) {
            const double hz = objective.analyzer().bandHz(b);
            cutoffHz = hz > 0.8 * objective.analyzer().bandHz(ref.numBands - 1) ? 20000.0 : 1.3 * hz;
            break;
        }
    }

    set(param::AmpAttackMs, std::max(1.0, attackMs));
    set(param::AmpDecayMs, std::max(5.0, decayMs));
    set(param::AmpSustain, sustain);
    set(param::AmpReleaseMs, std::max(5.0, releaseMs));
    set(param::FilterCutoffHz, std::clamp(cutoffHz, 100.0, 20000.0));
}

// The search itself: keeps the best point found so far and runs stages on it.
class Search {
public:
    Search(const SearchSpace& space, const Objective& objective, const MatchOptions& options, int budget)
        : space_(space), objective_(objective), options_(options), threads_(threadCount(options.threads)),
          budget_(std::max(1, budget)), enabled_(space.dims.size(), true)
    {
    }

    void disable(int paramIndex)
    {
        for (std::size_t i = 0; i < space_.dims.size(); ++i)
            if (space_.dims[i].param == paramIndex)
                enabled_[i] = false;
    }

    bool cancelled() const { return options_.cancel && options_.cancel->load(); }
    int evaluations() const { return evaluations_; }
    double bestDistance() const { return bestDistance_; }
    float bestGainDb() const { return bestGain_; }
    const std::vector<double>& best() const { return best_; }

    // Scores a batch of points (clamped into range) and remembers the best.
    std::vector<double> evaluate(const std::vector<std::vector<double>>& points)
    {
        std::vector<Objective::Score> scores(points.size());
        parallelFor(points.size(), threads_, [&](std::size_t i) {
            scores[i] = objective_.evaluate(space_.toParams(points[i]));
        });
        evaluations_ += static_cast<int>(points.size());

        std::vector<double> distances(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            distances[i] = scores[i].distance;
            if (scores[i].distance < bestDistance_) {
                bestDistance_ = scores[i].distance;
                bestGain_ = scores[i].gainDb;
                best_ = points[i];
                for (auto& v : best_)
                    v = std::clamp(v, 0.0, 1.0);
            }
        }
        report("Searching");
        return distances;
    }

    void start(const std::vector<std::vector<double>>& candidates) { evaluate(candidates); }

    // CMA-ES over the chosen dimensions, starting from `from` (or the best
    // point so far). `spread` is the starting step for each dimension, as a
    // fraction of its range. The other dimensions keep their values from `from`.
    void runStage(const std::vector<std::size_t>& dims, const std::vector<double>& spread, int stageBudget,
                  std::uint32_t seed, std::vector<double> from = {})
    {
        if (from.empty())
            from = best_;
        if (dims.empty() || stageBudget <= 0 || from.empty())
            return;

        const std::size_t n = dims.size();
        // More candidates per generation than the textbook default: the
        // landscape has many local dips, and they are evaluated in parallel anyway.
        const std::size_t population = std::max<std::size_t>(16, 6 + static_cast<std::size_t>(4.0 * std::log(static_cast<double>(n))));
        const int generations = std::max(1, stageBudget / static_cast<int>(population));

        // The optimizer works in units of each dimension's own step size.
        std::vector<double> mean(n);
        for (std::size_t k = 0; k < n; ++k)
            mean[k] = from[dims[k]] / spread[k];
        CmaEs cma(std::move(mean), 1.0, population, seed);

        const std::vector<double>& anchor = from; // the dimensions not in this stage stay put
        std::vector<std::vector<double>> points(population, anchor);
        std::vector<double> fitness(population);
        for (int g = 0; g < generations && !cancelled(); ++g) {
            const auto& asked = cma.ask();
            std::vector<double> penalty(population, 0.0);
            for (std::size_t i = 0; i < population; ++i) {
                points[i] = anchor;
                for (std::size_t k = 0; k < n; ++k) {
                    const double x = asked[i][k] * spread[k];
                    const double clamped = std::clamp(x, 0.0, 1.0);
                    points[i][dims[k]] = clamped;
                    // Candidates outside the range are scored at the edge, plus
                    // a cost for straying, which pulls the cloud back inside.
                    penalty[i] += (x - clamped) * (x - clamped);
                }
            }
            const auto distances = evaluate(points);
            for (std::size_t i = 0; i < population; ++i)
                fitness[i] = distances[i] + 30.0 * penalty[i];
            cma.tell(fitness);
            if (cma.sigma() < 1.0e-3)
                break; // converged
        }
    }

    // A final pattern search: nudge each continuous setting a little up and
    // down from the best point, keep what helps, then try all the helpful
    // nudges together. Repeated with smaller steps. CMA-ES finds the right
    // region; this settles the details.
    void polish(const std::vector<std::size_t>& dims, int polishBudget)
    {
        std::vector<std::size_t> continuous;
        for (const std::size_t d : dims)
            if (!space_.dims[d].isChoice())
                continuous.push_back(d);
        const int stop = evaluations_ + polishBudget;

        for (const double step : {0.04, 0.015, 0.005}) {
            for (int round = 0; round < 2 && !cancelled() && evaluations_ < stop && !best_.empty(); ++round) {
                const auto base = best_;
                const double baseDistance = bestDistance_;
                std::vector<std::vector<double>> points;
                std::vector<std::pair<std::size_t, double>> moves;
                for (const std::size_t d : continuous) {
                    for (const double s : {-step, step}) {
                        auto p = base;
                        p[d] = std::clamp(p[d] + s, 0.0, 1.0);
                        if (p[d] != base[d]) {
                            moves.push_back({d, p[d]});
                            points.push_back(std::move(p));
                        }
                    }
                }
                const auto distances = evaluate(points);

                // The better direction for each setting that improved.
                std::vector<std::pair<std::size_t, double>> helpful;
                std::vector<double> gain;
                for (std::size_t i = 0; i < moves.size(); ++i) {
                    if (distances[i] >= baseDistance)
                        continue;
                    bool replaced = false;
                    for (std::size_t h = 0; h < helpful.size(); ++h) {
                        if (helpful[h].first == moves[i].first) {
                            if (baseDistance - distances[i] > gain[h]) {
                                helpful[h] = moves[i];
                                gain[h] = baseDistance - distances[i];
                            }
                            replaced = true;
                        }
                    }
                    if (!replaced) {
                        helpful.push_back(moves[i]);
                        gain.push_back(baseDistance - distances[i]);
                    }
                }
                if (helpful.empty())
                    break; // nothing to gain at this step size
                if (helpful.size() > 1) {
                    auto combined = base;
                    for (const auto& [d, v] : helpful)
                        combined[d] = v;
                    evaluate({combined});
                }
            }
        }
    }

    // Tries every alternative for each choice dimension (waveform, filter type,
    // effect on/off, ...) with everything else held, and keeps improvements.
    void sweepChoices(const std::vector<std::size_t>& dims)
    {
        for (const std::size_t d : dims) {
            const auto& dim = space_.dims[d];
            if (!dim.isChoice() || best_.empty() || cancelled())
                continue;
            const std::size_t current = dim.choiceIndex(dim.toValue(best_[d]));
            std::vector<std::vector<double>> points;
            for (std::size_t c = 0; c < dim.choices.size(); ++c) {
                if (c == current)
                    continue;
                auto point = best_;
                point[d] = (static_cast<double>(c) + 0.5) / static_cast<double>(dim.choices.size());
                points.push_back(std::move(point));
            }
            evaluate(points);
        }
    }

    std::vector<std::size_t> dims(bool includeExtended) const
    {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < space_.dims.size(); ++i)
            if (enabled_[i] && (includeExtended || !space_.dims[i].extended))
                out.push_back(i);
        return out;
    }

    void report(const char* stage) const
    {
        if (!options_.onProgress)
            return;
        MatchProgress progress;
        progress.fraction = std::clamp(0.05 + 0.9 * static_cast<double>(evaluations_) / budget_, 0.0, 0.95);
        progress.distance = bestDistance_;
        progress.stage = stage;
        options_.onProgress(progress);
    }

private:
    const SearchSpace& space_;
    const Objective& objective_;
    const MatchOptions& options_;
    unsigned threads_;
    int budget_;
    std::vector<bool> enabled_;

    std::vector<double> best_;
    double bestDistance_ = 1.0e9;
    float bestGain_ = 0.0f;
    int evaluations_ = 0;
};

std::vector<float> matchedSection(const StereoBuffer& reference, double& rate, std::string& error)
{
    if (reference.left.empty()) {
        error = "The recording is empty.";
        return {};
    }
    if (reference.sampleRate < 16000.0 || reference.sampleRate > 400000.0) {
        error = "The recording's sample rate isn't supported (16 kHz to 384 kHz).";
        return {};
    }
    auto mono = toMono(reference.left, reference.right);
    rate = reference.sampleRate;
    while (rate > 50000.0) { // nothing above 24 kHz matters here
        mono = decimateBy2(mono);
        rate /= 2.0;
    }
    return mono;
}

// See simplifyPreset. `score` is the score of `params` (with `eq`) and is
// updated with each change kept. Together the changes cost at most `marginDb`.
void simplify(const Objective& objective, ParamValues& params, const MatchEqGains* eq, Objective::Score& score,
              double marginDb)
{
    const double limit = score.distance + marginDb;
    const auto tryInstead = [&](const ParamValues& changed) {
        const auto s = objective.evaluate(changed, eq);
        if (s.distance > limit)
            return false;
        params = changed;
        score = s;
        return true;
    };
    const auto value = [&](int index) { return params[static_cast<std::size_t>(index)]; };

    // A delay whose first echo comes after the recording ends can't be heard
    // in it, and a mix above 0.5 only turns the dry sound down (the gain makes
    // up for it). Either way the preset would play loud echoes the recording
    // doesn't have.
    if (value(param::DelayOn) >= 0.5f && !objective.notes().empty()) {
        const double firstEcho = objective.notes().front().startSec + 0.001 * value(param::DelayTimeMs);
        const bool tooWet = value(param::DelayMix) > 0.5f;
        if (tooWet || firstEcho >= objective.durationSec()) {
            auto off = params;
            off[param::DelayOn] = 0.0f;
            if (!tryInstead(off) && tooWet) {
                auto capped = params;
                capped[param::DelayMix] = 0.5f;
                tryInstead(capped);
            }
        }
    }

    // A pitch LFO that steps (square, saw, random), is slow, or goes deeper
    // than a semitone works as a hidden transpose: within the recording it
    // holds one pitch, but held notes jump when played from a keyboard.
    const auto target = static_cast<dsp::LfoTarget>(static_cast<int>(value(param::Lfo1Target)));
    const double depth = value(param::Lfo1Depth);
    if (target == dsp::LfoTarget::Pitch && depth > 0.0) {
        const auto wave = static_cast<dsp::LfoWave>(static_cast<int>(value(param::Lfo1Wave)));
        const bool stepped = wave == dsp::LfoWave::Square || wave == dsp::LfoWave::Saw || wave == dsp::LfoWave::Random;
        const double semitones = dsp::kLfoPitchSemitones * depth * depth;
        if (stepped || value(param::Lfo1RateHz) < 2.0f || semitones > 1.0) {
            auto off = params;
            off[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Off);
            if (!tryInstead(off)) {
                auto vibrato = params;
                vibrato[param::Lfo1Wave] = static_cast<float>(dsp::LfoWave::Sine);
                vibrato[param::Lfo1RateHz] = std::max(value(param::Lfo1RateHz), 2.0f);
                vibrato[param::Lfo1Depth] = static_cast<float>(std::min(depth, std::sqrt(1.0 / dsp::kLfoPitchSemitones)));
                tryInstead(vibrato);
            }
        }
    }
}

// How loud a preset sounds playing the objective's notes: the power in each
// band the matcher hears, A-weighted, so a deep sub counts for less than the
// same power in the midrange (as it does to the ear).
double loudnessDb(const Objective& objective, const ParamValues& params, const MatchEqGains* eq)
{
    const auto& analyzer = objective.analyzer();
    const auto spectrogram = analyzer.analyze(objective.renderMono(params, eq));
    std::vector<double> weight(spectrogram.numBands);
    for (std::size_t b = 0; b < weight.size(); ++b) {
        const double f2 = analyzer.bandHz(b) * analyzer.bandHz(b);
        const double a = 12194.0 * 12194.0 * f2 * f2 /
                         ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
        weight[b] = a * a;
    }
    double sum = 0.0;
    for (std::size_t f = 0; f < spectrogram.numFrames; ++f)
        for (std::size_t b = 0; b < spectrogram.numBands; ++b)
            sum += std::pow(10.0, spectrogram.at(f, b) / 10.0) * weight[b];
    return 10.0 * std::log10(sum + 1.0e-30);
}

// Moves the key so the preset's loudest oscillator plays the notes, to the
// nearest octave. Returns how far the notes moved up; `preset` and `notes`
// are changed to match, so the sound stays the same.
int normaliseKey(const Objective& objective, Preset& preset, std::vector<NoteEvent>& notes)
{
    // How loud each oscillator is on its own, through the rest of the preset.
    std::array<double, param::kNumOscs> powerDb{};
    int loudest = -1;
    const auto level = [&](const ParamValues& p, int k) { return p[static_cast<std::size_t>(param::osc(k, param::OscLevel))]; };
    for (int k = 0; k < param::kNumOscs; ++k) {
        if (level(preset.params, k) <= 0.02f)
            continue;
        auto solo = preset.params;
        for (int j = 0; j < param::kNumOscs; ++j)
            if (j != k)
                solo[static_cast<std::size_t>(param::osc(j, param::OscLevel))] = 0.0f;
        solo[param::NoiseLevel] = 0.0f;
        powerDb[static_cast<std::size_t>(k)] = loudnessDb(objective, solo, &preset.matchEq);
        if (loudest < 0 || powerDb[static_cast<std::size_t>(k)] > powerDb[static_cast<std::size_t>(loudest)])
            loudest = k;
    }
    if (loudest < 0)
        return 0;

    // Never below the note found: the recording repeats at the period of its
    // lowest part, so the key is at or above it.
    const auto octaveOf = [&](int k) {
        const float coarse = preset.params[static_cast<std::size_t>(param::osc(k, param::OscCoarse))];
        return std::max(0, 12 * static_cast<int>(std::lround(coarse / 12.0)));
    };
    int shift = octaveOf(loudest);
    // An oscillator within 1 dB of the loudest is as good a choice: take the
    // one that needs the smallest move, so a near tie doesn't flip the key.
    for (int k = 0; k < param::kNumOscs; ++k)
        if (level(preset.params, k) > 0.02f &&
            powerDb[static_cast<std::size_t>(k)] >= powerDb[static_cast<std::size_t>(loudest)] - 1.0 && octaveOf(k) < shift)
            shift = octaveOf(k);

    // Settle for a smaller move when the notes or the oscillators would leave their range.
    for (; shift > 0; shift -= 12) {
        bool fits = true;
        for (const auto& n : notes)
            fits = fits && n.note + shift >= 0 && n.note + shift <= 127;
        auto moved = preset.params;
        if (fits && moveKey(moved, shift)) {
            preset.params = moved;
            for (auto& n : notes)
                n.note += shift;
            break;
        }
    }
    return shift;
}

} // namespace

bool moveKey(ParamValues& params, int semitones)
{
    if (semitones == 0)
        return true;
    ParamValues moved = params;
    const auto& coarseSpec = spec(param::Osc1Coarse);
    for (int k = 0; k < param::kNumOscs; ++k) {
        const auto coarse = static_cast<std::size_t>(param::osc(k, param::OscCoarse));
        const float value = params[coarse] - static_cast<float>(semitones);
        if (value < coarseSpec.min || value > coarseSpec.max) {
            if (params[static_cast<std::size_t>(param::osc(k, param::OscLevel))] > 0.0f)
                return false;
            moved[coarse] = std::clamp(value, coarseSpec.min, coarseSpec.max); // silent: can't be heard
        } else {
            moved[coarse] = value;
        }
    }
    // The cutoff follows the note by filter_keytrack octaves per octave.
    const float keyTrack = params[param::FilterKeyTrack];
    if (keyTrack != 0.0f) {
        const auto& cutoffSpec = spec(param::FilterCutoffHz);
        const double cutoff = params[param::FilterCutoffHz] * std::exp2(-keyTrack * semitones / 12.0);
        if (cutoff < cutoffSpec.min || cutoff > cutoffSpec.max)
            return false;
        moved[param::FilterCutoffHz] = static_cast<float>(cutoff);
    }
    params = moved;
    return true;
}

double MatchResult::score() const { return 100.0 * std::exp(-distance / 10.0); }

MatchResult matchSound(const StereoBuffer& reference, const MatchOptions& options)
{
    MatchResult result;
    const auto progress = [&](double fraction, double distance, const char* stage) {
        if (options.onProgress)
            options.onProgress({fraction, distance, stage});
    };
    progress(0.0, 0.0, "Listening");

    double rate = 0.0;
    const auto mono = matchedSection(reference, rate, result.error);
    if (mono.empty())
        return result;

    const auto detection = detectNotes(mono, rate, options.maxSeconds);
    if (detection.notes.empty() || detection.numSamples < static_cast<std::size_t>(0.05 * rate)) {
        result.error = "Couldn't find a note in the recording. It may be silent or too short.";
        return result;
    }

    const double rateRatio = reference.sampleRate / rate;
    result.notes = detection.notes;
    result.sampleRate = rate;
    result.startSample = static_cast<std::size_t>(static_cast<double>(detection.startSample) * rateRatio);
    result.numSamples = static_cast<std::size_t>(static_cast<double>(detection.numSamples) * rateRatio);
    result.pitchConfidence = detection.confidence;

    const std::vector<float> section(mono.begin() + static_cast<std::ptrdiff_t>(detection.startSample),
                                     mono.begin() + static_cast<std::ptrdiff_t>(detection.startSample + detection.numSamples));

    // The search runs at half the sample rate when it can: half the rendering
    // and half the analysis for each candidate. The result is checked, and the
    // EQ match made, at the full rate.
    Objective full(section, rate, detection.notes);
    const bool halve = rate >= 44100.0;
    Objective search(halve ? decimateBy2(section) : section, halve ? rate / 2.0 : rate, detection.notes, false);

    // Vibrato and tremolo, measured once, so both objectives judge them the same way.
    const auto modulation = measureModulation(detection.pitch, search.reference(), search.analyzer(), detection.notes);
    full.useModulation(modulation);
    search.useModulation(modulation);
    result.modulation = modulation;

    const SearchSpace& space = SearchSpace::standard();

    // Key tracking can't be heard unless the recording has different pitches.
    bool onePitch = true;
    for (const auto& n : detection.notes)
        onePitch = onePitch && n.note == detection.notes.front().note;

    // Starting points: the measured envelope, brightness and modulation with
    // each waveform, and the encoder's guess when a trained model is available.
    ParamValues measured = space.base;
    applyMeasuredGuess(search, modulation, measured);
    std::vector<std::vector<double>> starts;
    for (const dsp::Wave wave : {dsp::Wave::Saw, dsp::Wave::Square, dsp::Wave::Triangle, dsp::Wave::Sine}) {
        for (const double brightness : {1.0, 0.35, 0.0}) {
            ParamValues p = measured;
            p[param::Osc1Wave] = static_cast<float>(wave);
            if (brightness == 0.0)
                p[param::FilterCutoffHz] = 20000.0f;
            else
                p[param::FilterCutoffHz] = clampToRange(spec(param::FilterCutoffHz),
                                                        static_cast<float>(measured[param::FilterCutoffHz] * brightness));
            starts.push_back(space.fromParams(p));
        }
    }
    const std::size_t encoderIndex = starts.size();
    if (options.encoder && options.encoder->valid()) {
        // The encoder listens to the first note only, up to where the next one starts.
        const double firstNoteEnd = detection.notes.size() > 1 ? detection.notes[1].startSec : -1.0;
        auto guess = options.encoder->predict(search.audio(), search.sampleRate(), detection.notes.front(), firstNoteEnd,
                                              detection.pitch);
        if (guess.size() == space.dims.size()) {
            // And the same guess with the measured vibrato or tremolo.
            if (modulation.found()) {
                auto withLfo = guess;
                for (std::size_t d = 0; d < space.dims.size(); ++d) {
                    const int index = space.dims[d].param;
                    if (index == param::Lfo1Target || index == param::Lfo1Wave || index == param::Lfo1RateHz ||
                        index == param::Lfo1Depth)
                        withLfo[d] = space.dims[d].fromValue(measured[static_cast<std::size_t>(index)]);
                }
                starts.push_back(std::move(guess));
                starts.push_back(std::move(withLfo));
            } else {
                starts.push_back(std::move(guess));
            }
        }
    }
    // Each attempt is a whole search from those starting points with its own
    // seed (seed, seed + 1, ...), and the closest result wins. Progress runs
    // from start to end once, across all of them.
    const int attempts = std::max(1, options.attempts);
    std::vector<std::vector<double>> found;
    std::vector<std::size_t> picked;
    double bestSoFar = 1.0e9;
    int evaluations = 0;
    for (int a = 0; a < attempts; ++a) {
        if (a > 0 && options.cancel && options.cancel->load())
            break;
        MatchOptions attempt = options;
        attempt.seed = options.seed + static_cast<std::uint32_t>(a);
        if (options.onProgress) {
            attempt.onProgress = [&options, a, attempts, bestSoFar](const MatchProgress& p) {
                const double within = std::clamp((p.fraction - 0.05) / 0.9, 0.0, 1.0);
                options.onProgress({0.05 + 0.9 * (static_cast<double>(a) + within) / static_cast<double>(attempts),
                                    std::min(p.distance, bestSoFar), p.stage});
            };
        }
        Search state(space, search, attempt, options.budget);
        if (onePitch)
            state.disable(param::FilterKeyTrack);

        const auto startDistances = state.evaluate(starts);
        if (a == 0) {
            result.startDistance = state.bestDistance();
            result.usedEncoder =
                startDistances.size() > encoderIndex &&
                *std::min_element(startDistances.begin() + static_cast<std::ptrdiff_t>(encoderIndex), startDistances.end()) <=
                    *std::min_element(startDistances.begin(), startDistances.end());

            // The best few starting points that differ from each other (another
            // waveform, or clearly different settings), so the search explores
            // more than one family of sounds.
            std::vector<std::size_t> order(starts.size());
            for (std::size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](std::size_t x, std::size_t y) { return startDistances[x] < startDistances[y]; });
            const auto waveDim = static_cast<std::size_t>(0); // osc1_wave is the first dimension
            for (const std::size_t i : order) {
                bool distinct = true;
                for (const std::size_t j : picked) {
                    double diff = 0.0;
                    for (std::size_t d = 0; d < space.dims.size(); ++d)
                        diff += std::abs(starts[i][d] - starts[j][d]);
                    const bool sameWave = space.dims[waveDim].choiceIndex(space.dims[waveDim].toValue(starts[i][waveDim])) ==
                                          space.dims[waveDim].choiceIndex(space.dims[waveDim].toValue(starts[j][waveDim]));
                    distinct = distinct && (!sameWave || diff / static_cast<double>(space.dims.size()) > 0.1);
                }
                if (distinct)
                    picked.push_back(i);
                if (picked.size() == 3)
                    break;
            }
        }

        // Stage 1: the core sound, from each of those starts, most effort on
        // the most promising. Stage 2: everything, from the best so far, with
        // the core only fine-tuned. Then a final polish.
        const int remaining = std::max(0, options.budget - state.evaluations());
        const auto core = state.dims(false), all = state.dims(true);
        const double shares[] = {0.24, 0.14, 0.10};
        for (std::size_t k = 0; k < picked.size(); ++k)
            state.runStage(core, std::vector<double>(core.size(), 0.25), static_cast<int>(shares[k] * remaining),
                           attempt.seed + 7u * static_cast<std::uint32_t>(k), starts[picked[k]]);
        state.sweepChoices(core);

        std::vector<double> spread(all.size());
        for (std::size_t k = 0; k < all.size(); ++k) {
            const auto& dim = space.dims[all[k]];
            spread[k] = dim.extended ? 0.25 : 0.08;
            // A measured vibrato or tremolo only needs fine-tuning: its rate is
            // close already (this step is about 7% of it), and the distance
            // is sharp around it.
            if (modulation.found() && dim.param == param::Lfo1RateHz)
                spread[k] = 0.02;
            if (modulation.found() && dim.param == param::Lfo1Depth)
                spread[k] = 0.05;
        }
        state.sweepChoices(all);
        const int polishBudget = static_cast<int>(0.1 * remaining);
        state.runStage(all, spread, std::max(0, options.budget - state.evaluations() - polishBudget - 60), attempt.seed + 1);
        state.sweepChoices(all);
        state.polish(all, std::max(0, options.budget - state.evaluations()));

        evaluations += state.evaluations();
        if (!state.best().empty()) {
            found.push_back(state.best());
            bestSoFar = std::min(bestSoFar, state.bestDistance());
        }
    }

    if (found.empty()) {
        result.error = "The search didn't produce a result.";
        return result;
    }

    // Final check and EQ match at the full sample rate, on the closest attempt.
    progress(0.96, bestSoFar, "Matching EQ");
    Preset preset;
    preset.name = "Matched";
    preset.params = space.toParams(found.front());

    auto score = full.evaluate(preset.params);
    for (std::size_t i = 1; i < found.size(); ++i) {
        const auto params = space.toParams(found[i]);
        const auto other = full.evaluate(params);
        if (other.distance < score.distance) {
            score = other;
            preset.params = params;
        }
    }
    // Drop what the recording gives no evidence for, while the gain is still
    // the search's (the level is fitted again for each change).
    simplify(full, preset.params, nullptr, score, kSimplifyMarginDb);
    const auto applyGain = [&](float gainDb) {
        preset.params[param::GainDb] = clampToRange(spec(param::GainDb), preset.params[param::GainDb] + gainDb);
    };
    applyGain(score.gainDb);
    result.searchDistance = score.distance;
    result.distance = score.distance;

    if (options.fitMatchEq) {
        const auto candidate = full.renderMono(preset.params);
        const auto curve = matchEqCurve(thirdOctaveSpectrum(full.audio(), rate), thirdOctaveSpectrum(candidate, rate), rate);
        const auto gains = fitMatchEq(curve, rate);
        const auto withEq = full.evaluate(preset.params, &gains);
        if (withEq.distance < result.distance) {
            preset.matchEq = gains;
            applyGain(withEq.gainDb);
            result.distance = withEq.distance;
        }
    }

    // The key: a sub-oscillator makes the recording repeat an octave or two
    // below the key that was played, and the notes found are that low. Move
    // them up to the loudest oscillator, and the oscillators down to match.
    result.keyShift = normaliseKey(full, preset, result.notes);

    result.preset = preset;
    result.evaluations = evaluations;
    result.ok = true;
    progress(1.0, result.distance, "Done");
    return result;
}

std::vector<float> encoderInputFor(const StereoBuffer& recording, double maxSeconds)
{
    double rate = 0.0;
    std::string error;
    const auto mono = matchedSection(recording, rate, error);
    if (mono.empty())
        return {};
    const auto detection = detectNotes(mono, rate, maxSeconds);
    if (detection.notes.empty())
        return {};
    std::vector<float> section(mono.begin() + static_cast<std::ptrdiff_t>(detection.startSample),
                               mono.begin() + static_cast<std::ptrdiff_t>(detection.startSample + detection.numSamples));
    // The same steps as in matchSound: halve the rate when the search does.
    if (rate >= 44100.0) {
        section = decimateBy2(section);
        rate /= 2.0;
    }
    const double firstNoteEnd = detection.notes.size() > 1 ? detection.notes[1].startSec : -1.0;
    return encoderFeatures(section, rate, detection.notes.front(), firstNoteEnd, detection.pitch);
}

namespace {

// An objective for a whole recording and given notes, judging modulation as matchSound does.
Objective objectiveFor(const std::vector<float>& mono, double rate, std::span<const NoteEvent> notes)
{
    Objective objective(mono, rate, std::vector<NoteEvent>(notes.begin(), notes.end()));
    objective.useModulation(measureModulation(trackPitch(mono, rate), objective.reference(), objective.analyzer(), notes));
    return objective;
}

} // namespace

double presetDistance(const StereoBuffer& reference, const Preset& preset, std::span<const NoteEvent> notes)
{
    double rate = 0.0;
    std::string error;
    const auto mono = matchedSection(reference, rate, error);
    if (mono.empty())
        return 1.0e3;
    return objectiveFor(mono, rate, notes).evaluate(preset.params, &preset.matchEq, false).distance;
}

Preset simplifyPreset(const StereoBuffer& reference, const Preset& preset, std::span<const NoteEvent> notes, double marginDb)
{
    double rate = 0.0;
    std::string error;
    const auto mono = matchedSection(reference, rate, error);
    if (mono.empty() || notes.empty())
        return preset;
    const auto objective = objectiveFor(mono, rate, notes);
    Preset out = preset;
    const auto before = objective.evaluate(out.params, &out.matchEq);
    auto score = before;
    simplify(objective, out.params, &out.matchEq, score, marginDb);
    // Keep the level: make up for what the changes added or took away.
    out.params[param::GainDb] = clampToRange(spec(param::GainDb), out.params[param::GainDb] + score.gainDb - before.gainDb);
    return out;
}

} // namespace synthmatch::match
