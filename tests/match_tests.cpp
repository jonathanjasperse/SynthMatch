// Tests for the matcher: analysis, note detection, the search, the EQ match
// and the encoder. Plain asserts, no framework; run through CTest.

#include "synthmatch/Render.h"
#include "synthmatch/dsp/MatchEq.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Fft.h"
#include "synthmatch/match/Matcher.h"
#include "synthmatch/match/Optimizer.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/SearchSpace.h"
#include "synthmatch/match/Train.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                                               \
        }                                                                                               \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                           \
    do {                                                                                                \
        const double a_ = (a), b_ = (b);                                                                \
        if (!(std::abs(a_ - b_) <= (tol))) {                                                            \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, expected %g +/- %g\n", __FILE__,     \
                         __LINE__, #a, a_, b_, static_cast<double>(tol));                               \
            ++g_failures;                                                                               \
        }                                                                                               \
    } while (0)

using namespace synthmatch;
using namespace synthmatch::match;

constexpr double kPi = std::numbers::pi;

std::vector<float> sine(double hz, double sampleRate, double seconds, double amplitude = 1.0)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * sampleRate));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sampleRate));
    return x;
}

double amplitudeAt(const std::vector<float>& x, double hz, double sampleRate, std::size_t from, std::size_t to)
{
    double re = 0.0, im = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double a = 2.0 * kPi * hz * static_cast<double>(i) / sampleRate;
        re += x[i] * std::cos(a);
        im -= x[i] * std::sin(a);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(to - from);
}

StereoBuffer renderNotes(const ParamValues& params, std::vector<NoteEvent> notes, double tail = 0.5,
                         double sampleRate = 48000.0)
{
    RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.tailSec = tail;
    return render(params, notes, settings);
}

// ---------------------------------------------------------------- analysis

void testFft()
{
    const std::size_t n = 1024;
    const Fft fft(n);
    std::vector<std::complex<float>> data(n);
    for (std::size_t i = 0; i < n; ++i)
        data[i] = {static_cast<float>(std::cos(2.0 * kPi * 37.0 * static_cast<double>(i) / n)), 0.0f};
    double energy = 0.0;
    for (const auto& v : data)
        energy += std::norm(v);

    fft.forward(data.data());

    // A cosine at bin 37 puts half its amplitude in bin 37 and half in its mirror.
    CHECK_NEAR(std::abs(data[37]), n / 2.0, 0.01);
    CHECK_NEAR(std::abs(data[n - 37]), n / 2.0, 0.01);
    CHECK(std::abs(data[36]) < 0.01f);
    CHECK(std::abs(data[100]) < 0.01f);

    // Energy is preserved (Parseval).
    double spectrumEnergy = 0.0;
    for (const auto& v : data)
        spectrumEnergy += std::norm(v);
    CHECK_NEAR(spectrumEnergy / n, energy, 1e-2);
}

void testSpectrogram()
{
    const double sr = 48000.0;
    const SpectrogramAnalyzer analyzer(sr);
    const auto tone = sine(1000.0, sr, 0.5);
    const auto spec = analyzer.analyze(tone);

    CHECK(spec.numBands == 64);
    CHECK(spec.numFrames == (tone.size() + 511) / 512);

    // The loudest band in the middle of the tone is the one nearest 1 kHz.
    const std::size_t frame = spec.numFrames / 2;
    std::size_t loudest = 0;
    for (std::size_t b = 1; b < spec.numBands; ++b)
        if (spec.at(frame, b) > spec.at(frame, loudest))
            loudest = b;
    CHECK(std::abs(analyzer.bandHz(loudest) - 1000.0) < 80.0);
    CHECK_NEAR(spec.at(frame, loudest), 0.0, 3.0); // a full-scale sine reads about 0 dB

    // Identical sounds are at distance zero; a quieter copy is the same once the level is lined up.
    CHECK_NEAR(spectralDistance(spec, spec), 0.0, 1e-9);
    const auto quiet = analyzer.analyze(sine(1000.0, sr, 0.5, 0.5));
    CHECK(spectralDistance(spec, quiet) > 3.0);
    CHECK_NEAR(bestGainDb(spec, quiet), 6.02, 0.1);
    CHECK_NEAR(spectralDistance(spec, quiet, bestGainDb(spec, quiet)), 0.0, 0.1);

    // A different pitch is far away; a slightly different one is closer.
    const double near = spectralDistance(spec, analyzer.analyze(sine(1100.0, sr, 0.5)));
    const double far = spectralDistance(spec, analyzer.analyze(sine(4000.0, sr, 0.5)));
    CHECK(near > 1.0);
    CHECK(far > near);
}

void testDecimate()
{
    const double sr = 48000.0;
    const auto low = decimateBy2(sine(1000.0, sr, 0.2));
    CHECK(low.size() == static_cast<std::size_t>(0.2 * sr) / 2);
    CHECK_NEAR(amplitudeAt(low, 1000.0, sr / 2.0, 200, low.size() - 200), 1.0, 0.01);

    // 15 kHz would fold down to 9 kHz at half the rate; the filter removes it first.
    const auto high = decimateBy2(sine(15000.0, sr, 0.2));
    CHECK(amplitudeAt(high, 9000.0, sr / 2.0, 200, high.size() - 200) < 0.01);
}

// ---------------------------------------------------------------- notes

void testPitchAndNotes()
{
    auto p = defaultValues();
    p[param::AmpReleaseMs] = 80.0f;

    // Single notes across the keyboard, with each waveform.
    int index = 0;
    for (const int note : {33, 45, 57, 69, 81, 93}) {
        auto patch = p;
        patch[param::Osc1Wave] = static_cast<float>(index++ % 4);
        const auto audio = renderNotes(patch, {{0.1, 0.6, note, 1.0f}});
        const auto found = detectNotes(toMono(audio.left, audio.right), audio.sampleRate);
        CHECK(found.notes.size() == 1);
        if (found.notes.size() == 1) {
            CHECK(found.notes[0].note == note);
            CHECK_NEAR(found.notes[0].durationSec, 0.6, 0.08);
            CHECK(found.confidence > 0.8);
            // The analysed section starts where the note does.
            CHECK_NEAR(static_cast<double>(found.startSample) / audio.sampleRate, 0.1, 0.01);
            CHECK(found.notes[0].startSec == 0.0);
        }
    }

    // A phrase: two separate notes, then two played legato, then a repeated note.
    const std::vector<NoteEvent> phrase{{0.0, 0.25, 48, 1.0f},  {0.4, 0.25, 55, 0.6f}, {0.8, 0.3, 60, 1.0f},
                                        {1.1, 0.3, 64, 1.0f},   {1.6, 0.15, 52, 1.0f}, {1.85, 0.15, 52, 1.0f}};
    const auto audio = renderNotes(p, phrase);
    const auto found = detectNotes(toMono(audio.left, audio.right), audio.sampleRate);
    CHECK(found.notes.size() == phrase.size());
    if (found.notes.size() == phrase.size()) {
        for (std::size_t i = 0; i < phrase.size(); ++i) {
            CHECK(found.notes[i].note == phrase[i].note);
            CHECK_NEAR(found.notes[i].startSec, phrase[i].startSec, 0.03);
        }
        CHECK_NEAR(found.notes[0].durationSec, 0.25, 0.06);
        CHECK_NEAR(found.notes[4].durationSec, 0.15, 0.06); // ends before the same note is struck again
        CHECK(found.notes[1].velocity < 0.8f); // the quieter note
        CHECK(found.notes[0].velocity > 0.9f);
    }

    // Only the first few seconds are used when asked.
    const auto limited = detectNotes(toMono(audio.left, audio.right), audio.sampleRate, 1.0);
    CHECK(limited.notes.size() == 3);
    CHECK(limited.numSamples <= static_cast<std::size_t>(1.01 * audio.sampleRate));

    // Silence has no notes.
    const std::vector<float> silence(48000, 0.0f);
    CHECK(detectNotes(silence, 48000.0).notes.empty());
}

// ---------------------------------------------------------------- EQ match

void testMatchEqFit()
{
    const double sr = 48000.0;

    // A smooth target curve: a dip in the bass and a bump around 2 kHz.
    MatchEqGains target{};
    for (int band = 0; band < kMatchEqBands; ++band) {
        const double octaves = std::log2(dsp::matchEqBandHz(band) / 1000.0);
        target[static_cast<std::size_t>(band)] =
            static_cast<float>(5.0 * std::exp(-0.5 * (octaves - 1.0) * (octaves - 1.0)) -
                               4.0 * std::exp(-0.5 * (octaves + 3.0) * (octaves + 3.0)));
    }

    // Using the target directly as band gains overshoots, because bands overlap...
    const auto naive = dsp::matchEqCoeffs(sr, target);
    double naiveError = 0.0, fittedError = 0.0;
    const auto gains = fitMatchEq(target, sr);
    const auto fitted = dsp::matchEqCoeffs(sr, gains);
    for (int band = 2; band < kMatchEqBands - 2; ++band) {
        const double hz = dsp::matchEqBandHz(band);
        const double want = target[static_cast<std::size_t>(band)];
        naiveError = std::max(naiveError, std::abs(dsp::matchEqGainDbAt(naive, hz, sr) - want));
        fittedError = std::max(fittedError, std::abs(dsp::matchEqGainDbAt(fitted, hz, sr) - want));
    }
    // ...and the fit corrects for that.
    CHECK(naiveError > 1.5);
    CHECK(fittedError < 0.3);

    // End to end: noise through a known EQ, compared with the same noise without it.
    auto p = defaultValues();
    p[param::Osc1Level] = 0.0f;
    p[param::NoiseLevel] = 1.0f;
    Preset coloured;
    coloured.params = p;
    coloured.matchEq = gains;
    const std::vector<NoteEvent> notes{{0.0, 2.0, 60, 1.0f}};
    RenderSettings settings;
    settings.sampleRate = sr;
    settings.tailSec = 0.1;
    const auto reference = render(coloured, notes, settings);
    const auto plain = render(p, notes, settings);

    const auto curve = matchEqCurve(thirdOctaveSpectrum(toMono(reference.left, reference.right), sr),
                                    thirdOctaveSpectrum(toMono(plain.left, plain.right), sr), sr);
    // The curve keeps only the shape: compare after removing each one's middle value.
    const auto centred = [](MatchEqGains g, int from, int to) {
        std::vector<float> v(g.begin() + from, g.begin() + to);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        const float mid = v[v.size() / 2];
        for (auto& x : g)
            x -= mid;
        return g;
    };
    const auto a = centred(curve, 6, 28), b = centred(target, 6, 28);
    for (int band = 6; band < 28; ++band) // 80 Hz to 10 kHz
        CHECK_NEAR(a[static_cast<std::size_t>(band)], b[static_cast<std::size_t>(band)], 1.5);
}

// ---------------------------------------------------------------- optimizer

void testEigen()
{
    const std::size_t n = 6;
    std::vector<double> a(n * n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j <= i; ++j)
            a[i * n + j] = a[j * n + i] = std::sin(1.7 * static_cast<double>(i * n + j)) + (i == j ? 3.0 : 0.0);

    std::vector<double> vectors, values;
    symmetricEigen(a, n, vectors, values);

    // V * diag(values) * V^T rebuilds the matrix, and V's columns are orthonormal.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            double rebuilt = 0.0, dot = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                rebuilt += vectors[i * n + k] * values[k] * vectors[j * n + k];
                dot += vectors[k * n + i] * vectors[k * n + j];
            }
            CHECK_NEAR(rebuilt, a[i * n + j], 1e-9);
            CHECK_NEAR(dot, i == j ? 1.0 : 0.0, 1e-9);
        }
    }
}

void testCmaEs()
{
    const auto minimise = [](auto&& function, std::vector<double> start, int generations) {
        CmaEs cma(std::move(start), 0.5, 0, 7);
        double best = 1.0e30;
        std::vector<double> fitness(cma.population());
        for (int g = 0; g < generations; ++g) {
            const auto& points = cma.ask();
            for (std::size_t i = 0; i < points.size(); ++i) {
                fitness[i] = function(points[i]);
                best = std::min(best, fitness[i]);
            }
            cma.tell(fitness);
        }
        return best;
    };

    // A simple bowl in 10 dimensions.
    const auto sphere = [](const std::vector<double>& x) {
        double sum = 0.0;
        for (const double v : x)
            sum += (v - 0.3) * (v - 0.3);
        return sum;
    };
    CHECK(minimise(sphere, std::vector<double>(10, 2.0), 300) < 1e-8);

    // A stretched bowl: one direction matters a million times more than another.
    // Learning the shape (the covariance) is what makes this solvable.
    const auto ellipsoid = [](const std::vector<double>& x) {
        double sum = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i)
            sum += std::pow(1.0e6, static_cast<double>(i) / static_cast<double>(x.size() - 1)) * x[i] * x[i];
        return sum;
    };
    CHECK(minimise(ellipsoid, std::vector<double>(8, 1.0), 600) < 1e-6);

    // Rosenbrock's curved valley.
    const auto rosenbrock = [](const std::vector<double>& x) {
        double sum = 0.0;
        for (std::size_t i = 0; i + 1 < x.size(); ++i)
            sum += 100.0 * std::pow(x[i + 1] - x[i] * x[i], 2.0) + std::pow(1.0 - x[i], 2.0);
        return sum;
    };
    CHECK(minimise(rosenbrock, std::vector<double>(5, 0.0), 800) < 1e-6);
}

// ---------------------------------------------------------------- search space

void testSearchSpace()
{
    const SearchSpace& space = SearchSpace::standard();

    std::set<int> seen;
    std::size_t outputs = 0;
    for (const auto& dim : space.dims) {
        CHECK(seen.insert(dim.param).second); // each parameter appears once
        outputs += dim.isChoice() ? dim.choices.size() : 1;
        for (const float c : dim.choices)
            CHECK(c >= spec(dim.param).min && c <= spec(dim.param).max);
    }
    CHECK(space.outputSize() == outputs);

    // A point in the space turns into parameters and back.
    std::vector<double> x(space.dims.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = 0.07 + 0.86 * static_cast<double>((i * 37) % 100) / 100.0;
    const auto params = space.toParams(x);
    const auto back = space.fromParams(params);
    const auto again = space.toParams(back);
    for (int i = 0; i < kNumParams; ++i)
        CHECK_NEAR(again[static_cast<std::size_t>(i)], params[static_cast<std::size_t>(i)],
                   1e-3 * (spec(i).max - spec(i).min));

    // Parameters outside the search keep their fixed values.
    CHECK(params[param::GainDb] == kSearchGainDb);
    CHECK(params[param::EqOn] == 0.0f);
    CHECK(params[param::Osc1Coarse] == 0.0f);

    // What can be heard: a silent oscillator's settings and a switched-off effect's can't.
    auto p = space.base;
    p[param::Osc2Level] = 0.0f;
    p[param::ReverbOn] = 0.0f;
    p[param::DelayOn] = 1.0f;
    const auto audible = audibleDims(space, p);
    for (std::size_t i = 0; i < space.dims.size(); ++i) {
        const int index = space.dims[i].param;
        if (index == param::Osc2Wave || index == param::Osc2Coarse || index == param::ReverbDecaySec ||
            index == param::Osc1PulseWidth)
            CHECK(!audible[i]);
        if (index == param::Osc2Level || index == param::Osc1Wave || index == param::DelayTimeMs ||
            index == param::FilterCutoffHz || index == param::ReverbOn)
            CHECK(audible[i]);
    }
}

// ---------------------------------------------------------------- matching

void testMatchRecoversSound()
{
    // A square-wave bass with a filter sweep. The matcher only hears the audio.
    auto p = defaultValues();
    p[param::GainDb] = -6.0f;
    p[param::Osc1Wave] = static_cast<float>(dsp::Wave::Square);
    p[param::FilterCutoffHz] = 900.0f;
    p[param::FilterResonance] = 0.4f;
    p[param::FilterEnvAmount] = 3.0f;
    p[param::FilterDecayMs] = 400.0f;
    p[param::FilterSustain] = 0.2f;
    p[param::AmpReleaseMs] = 300.0f;
    const auto reference = renderNotes(p, {{0.05, 0.8, 45, 1.0f}}, 0.8);

    MatchOptions options;
    options.budget = 4000;
    options.threads = 2;
    int calls = 0;
    double lastFraction = 0.0;
    options.onProgress = [&](const MatchProgress& progress) {
        ++calls;
        CHECK(progress.fraction >= lastFraction - 1e-9); // progress never goes backwards
        lastFraction = progress.fraction;
    };

    const auto result = matchSound(reference, options);
    CHECK(result.ok);
    CHECK(calls > 10);
    CHECK_NEAR(lastFraction, 1.0, 1e-9);
    if (!result.ok)
        return;

    std::printf("match test: distance %.2f dB from a start of %.2f dB, %d sounds tried\n", result.distance,
                result.startDistance, result.evaluations);

    CHECK(result.notes.size() == 1 && result.notes[0].note == 45);
    CHECK(result.distance < 0.6 * result.startDistance);
    CHECK(result.distance < 4.0);
    CHECK(result.evaluations <= options.budget + 200);
    CHECK(result.score() > 60.0);

    // The broad character is right, and the level too.
    const auto& found = result.preset.params;
    CHECK(found[param::Osc1Wave] == static_cast<float>(dsp::Wave::Square));
    CHECK(found[param::FilterType] == 0.0f);
    CHECK(found[param::FilterCutoffHz] < 4000.0f);
    CHECK_NEAR(found[param::GainDb], -6.0, 4.0);

    // The reported distance is what an independent check measures.
    CHECK_NEAR(presetDistance({reference.sampleRate,
                               {reference.left.begin() + static_cast<std::ptrdiff_t>(result.startSample),
                                reference.left.begin() + static_cast<std::ptrdiff_t>(result.startSample + result.numSamples)},
                               {reference.right.begin() + static_cast<std::ptrdiff_t>(result.startSample),
                                reference.right.begin() + static_cast<std::ptrdiff_t>(result.startSample + result.numSamples)}},
                              result.preset, result.notes),
               result.distance, 0.5);

    // The same seed gives the same answer.
    options.onProgress = nullptr;
    const auto again = matchSound(reference, options);
    CHECK(again.ok && again.preset.params == result.preset.params);

    // Cancelling stops early but still returns a usable result.
    std::atomic<bool> cancel{false};
    options.cancel = &cancel;
    options.onProgress = [&](const MatchProgress& progress) {
        if (progress.fraction > 0.2)
            cancel = true;
    };
    const auto partial = matchSound(reference, options);
    CHECK(partial.ok);
    CHECK(partial.evaluations < result.evaluations);
}

void testMatchEqHelps()
{
    // The same patch recorded through an EQ the synth doesn't have: the search
    // can't reproduce the colouring, the EQ match can.
    Preset coloured;
    coloured.params[param::FilterCutoffHz] = 4000.0f;
    for (int band = 0; band < kMatchEqBands; ++band) {
        const double octaves = std::log2(dsp::matchEqBandHz(band) / 1000.0);
        coloured.matchEq[static_cast<std::size_t>(band)] = static_cast<float>(8.0 * std::exp(-2.0 * octaves * octaves));
    }
    const std::vector<NoteEvent> notes{{0.0, 0.7, 40, 1.0f}};
    RenderSettings settings;
    settings.tailSec = 0.4;
    const auto reference = render(coloured, notes, settings);

    MatchOptions options;
    options.budget = 2500;
    options.threads = 2;
    const auto withEq = matchSound(reference, options);
    CHECK(withEq.ok);
    if (withEq.ok) {
        CHECK(withEq.distance <= withEq.searchDistance);
        float largest = 0.0f;
        for (const float g : withEq.preset.matchEq)
            largest = std::max(largest, std::abs(g));
        CHECK(largest > 1.0f); // a curve was stored
    }

    options.fitMatchEq = false;
    const auto without = matchSound(reference, options);
    CHECK(without.ok);
    if (without.ok)
        for (const float g : without.preset.matchEq)
            CHECK(g == 0.0f);
}

void testMatchRejectsBadInput()
{
    StereoBuffer silent;
    silent.sampleRate = 48000.0;
    silent.left.assign(48000, 0.0f);
    silent.right = silent.left;
    const auto result = matchSound(silent);
    CHECK(!result.ok);
    CHECK(!result.error.empty());

    CHECK(!matchSound(StereoBuffer{}).ok);

    StereoBuffer odd = silent;
    odd.sampleRate = 4000.0;
    CHECK(!matchSound(odd).ok);
}

// ---------------------------------------------------------------- encoder

void testEncoderFeatures()
{
    const SearchSpace& space = SearchSpace::standard();
    const auto featuresOf = [&](std::uint32_t seed) {
        const auto sound = randomSound(space, seed);
        const auto audio = renderNotes(sound.params, {sound.note}, sound.seconds - sound.note.durationSec, 24000.0);
        return encoderFeatures(toMono(audio.left, audio.right), audio.sampleRate, sound.note);
    };

    const auto a = featuresOf(1), b = featuresOf(2);
    CHECK(a.size() == encoderFeatureCount());
    for (const float v : a)
        CHECK(v >= 0.0f && v <= 1.0f);
    CHECK(a == featuresOf(1)); // deterministic
    CHECK(a != b);

    // Random sounds stay inside every parameter's range, and differ.
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        const auto sound = randomSound(space, seed);
        for (int i = 0; i < kNumParams; ++i)
            CHECK(sound.params[static_cast<std::size_t>(i)] == clampToRange(spec(i), sound.params[static_cast<std::size_t>(i)]));
        CHECK(sound.note.note >= 28 && sound.note.note <= 88);
    }
    CHECK(randomSound(space, 3).params != randomSound(space, 4).params);

    // Silence in gives all zeros rather than garbage.
    const std::vector<float> silence(24000, 0.0f);
    const auto none = encoderFeatures(silence, 24000.0, {0.0, 0.5, 60, 1.0f});
    CHECK(none.size() == encoderFeatureCount());
    for (const float v : none)
        CHECK(std::isfinite(v));
}

void testTrainingAndModelFile()
{
    // A tiny training run: far too small to be useful, but the loss must fall
    // and the model must save, load and predict.
    TrainOptions options;
    options.samples = 700;
    options.epochs = 6;
    options.hiddenSize = 48;
    options.batchSize = 32;
    options.threads = 2;
    std::vector<std::string> lines;
    options.log = [&](const std::string& line) { lines.push_back(line); };

    const auto result = trainEncoder(options);
    CHECK(result.ok);
    if (!result.ok)
        return;
    CHECK(result.report.epochLoss.size() == 6);
    CHECK(result.report.epochLoss.back() < 0.9 * result.report.epochLoss.front());
    CHECK(std::isfinite(result.report.validationLoss));
    CHECK(result.report.dimScore.size() == SearchSpace::standard().dims.size());
    CHECK(lines.size() >= 8);

    const auto path = std::filesystem::temp_directory_path() / "synthmatch_test_model.smm";
    std::string error;
    CHECK(result.encoder.save(path, &error));
    const auto loaded = Encoder::load(path, &error);
    CHECK(loaded.has_value());

    const SearchSpace& space = SearchSpace::standard();
    const auto sound = randomSound(space, 12345);
    const auto audio = renderNotes(sound.params, {sound.note}, 1.0, 24000.0);
    const auto mono = toMono(audio.left, audio.right);
    const auto guess = result.encoder.predict(mono, audio.sampleRate, sound.note);
    CHECK(guess.size() == space.dims.size());
    for (const double v : guess)
        CHECK(v >= 0.0 && v <= 1.0);
    if (loaded)
        CHECK(loaded->predict(mono, audio.sampleRate, sound.note) == guess);

    // A recording's encoder input, prepared as matching prepares it, can be
    // added to training as a graded example.
    const auto input = encoderInputFor(audio);
    CHECK(input.size() == encoderFeatureCount());
    TrainOptions withGraded = options;
    withGraded.samples = 300;
    withGraded.epochs = 1;
    withGraded.extra.push_back({input, sound.params});
    withGraded.extraRepeat = 5;
    lines.clear();
    CHECK(trainEncoder(withGraded).ok);
    bool mentioned = false;
    for (const auto& line : lines)
        mentioned = mentioned || line.find("1 graded recording") != std::string::npos;
    CHECK(mentioned);
    StereoBuffer silence;
    silence.sampleRate = 48000.0;
    silence.left.assign(24000, 0.0f);
    silence.right = silence.left;
    CHECK(encoderInputFor(silence).empty());

    // The matcher accepts the model for its starting guess.
    MatchOptions match;
    match.budget = 600;
    match.threads = 2;
    match.encoder = &result.encoder;
    CHECK(matchSound(audio, match).ok);

    // Damaged or foreign files are refused with a message.
    std::vector<unsigned char> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(Encoder::fromMemory(bytes).has_value());
    CHECK(!Encoder::fromMemory(std::span(bytes).first(bytes.size() / 2), &error));
    CHECK(!error.empty());
    bytes[0] = 'X';
    CHECK(!Encoder::fromMemory(bytes, &error));
    std::filesystem::remove(path);
    CHECK(!Encoder::load(path, &error));
}

} // namespace

int main()
{
    testFft();
    testSpectrogram();
    testDecimate();
    testPitchAndNotes();
    testMatchEqFit();
    testEigen();
    testCmaEs();
    testSearchSpace();
    testMatchRecoversSound();
    testMatchEqHelps();
    testMatchRejectsBadInput();
    testEncoderFeatures();
    testTrainingAndModelFile();

    if (g_failures == 0)
        std::puts("All match tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
