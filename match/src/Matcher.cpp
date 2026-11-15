#include "synthmatch/match/Matcher.h"

#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Optimizer.h"
#include "synthmatch/match/Parallel.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/SearchSpace.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace synthmatch::match {

namespace {

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

    Objective(std::vector<float> reference, double sampleRate, std::vector<NoteEvent> notes)
        : audio_(std::move(reference)), sampleRate_(sampleRate), notes_(std::move(notes)),
          analyzer_(sampleRate, configFor(sampleRate)), reference_(analyzer_.analyze(audio_))
    {
        double lastOff = 0.0;
        for (const auto& n : notes_)
            lastOff = std::max(lastOff, n.startSec + n.durationSec);
        settings_.sampleRate = sampleRate_;
        // Render exactly as much as the reference holds.
        settings_.tailSec = std::max(0.0, static_cast<double>(audio_.size()) / sampleRate_ - lastOff);
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
        score.distance = spectralDistance(reference_, spectrogram, score.gainDb);
        return score;
    }

    const std::vector<float>& audio() const { return audio_; }
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
};

// Rough envelope and brightness measurements, to start the search somewhere sensible.
void applyMeasuredGuess(const Objective& objective, ParamValues& params)
{
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

    const auto set = [&](int index, double value) {
        params[static_cast<std::size_t>(index)] = clampToRange(spec(index), static_cast<float>(value));
    };
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

} // namespace

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
    const Objective full(section, rate, detection.notes);
    const bool halve = rate >= 44100.0;
    const Objective search(halve ? decimateBy2(section) : section, halve ? rate / 2.0 : rate, detection.notes);

    const SearchSpace& space = SearchSpace::standard();
    Search state(space, search, options, options.budget);

    // Key tracking can't be heard unless the recording has different pitches.
    bool onePitch = true;
    for (const auto& n : detection.notes)
        onePitch = onePitch && n.note == detection.notes.front().note;
    if (onePitch)
        state.disable(param::FilterKeyTrack);

    // Starting points: the measured envelope and brightness with each
    // waveform, and the encoder's guess when a trained model is available.
    ParamValues measured = space.base;
    applyMeasuredGuess(search, measured);
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
        auto guess = options.encoder->predict(search.audio(), search.sampleRate(), detection.notes.front(), firstNoteEnd);
        if (guess.size() == space.dims.size())
            starts.push_back(std::move(guess));
    }
    const auto startDistances = state.evaluate(starts);
    result.startDistance = state.bestDistance();
    result.usedEncoder = startDistances.size() > encoderIndex &&
                         startDistances[encoderIndex] <= *std::min_element(startDistances.begin(), startDistances.end());

    // The best few starting points that differ from each other (another
    // waveform, or clearly different settings), so the search explores more
    // than one family of sounds.
    std::vector<std::size_t> order(starts.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return startDistances[a] < startDistances[b]; });
    const auto waveDim = static_cast<std::size_t>(0); // osc1_wave is the first dimension
    std::vector<std::size_t> picked;
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

    // Stage 1: the core sound, from each of those starts, most effort on the
    // most promising. Stage 2: everything, from the best so far, with the core
    // only fine-tuned. Then a final polish.
    const int remaining = std::max(0, options.budget - state.evaluations());
    const auto core = state.dims(false), all = state.dims(true);
    const double shares[] = {0.24, 0.14, 0.10};
    for (std::size_t k = 0; k < picked.size(); ++k)
        state.runStage(core, std::vector<double>(core.size(), 0.25), static_cast<int>(shares[k] * remaining),
                       options.seed + 7u * static_cast<std::uint32_t>(k), starts[picked[k]]);
    state.sweepChoices(core);

    std::vector<double> spread(all.size());
    for (std::size_t k = 0; k < all.size(); ++k)
        spread[k] = space.dims[all[k]].extended ? 0.25 : 0.08;
    state.sweepChoices(all);
    const int polishBudget = static_cast<int>(0.1 * remaining);
    state.runStage(all, spread, std::max(0, options.budget - state.evaluations() - polishBudget - 60), options.seed + 1);
    state.sweepChoices(all);
    state.polish(all, std::max(0, options.budget - state.evaluations()));

    if (state.best().empty()) {
        result.error = "The search didn't produce a result.";
        return result;
    }

    // Final check and EQ match at the full sample rate.
    progress(0.96, state.bestDistance(), "Matching EQ");
    Preset preset;
    preset.name = "Matched";
    preset.params = space.toParams(state.best());

    auto score = full.evaluate(preset.params);
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

    result.preset = preset;
    result.evaluations = state.evaluations();
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
    return encoderFeatures(section, rate, detection.notes.front(), firstNoteEnd);
}

double presetDistance(const StereoBuffer& reference, const Preset& preset, std::span<const NoteEvent> notes)
{
    double rate = 0.0;
    std::string error;
    const auto mono = matchedSection(reference, rate, error);
    if (mono.empty())
        return 1.0e3;
    const Objective objective(mono, rate, std::vector<NoteEvent>(notes.begin(), notes.end()));
    return objective.evaluate(preset.params, &preset.matchEq, false).distance;
}

} // namespace synthmatch::match
