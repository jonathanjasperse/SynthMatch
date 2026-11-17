// Tests for the matcher: analysis, note detection, the search, the EQ match
// and the encoder. Plain asserts, no framework; run through CTest.

#include "synthmatch/Render.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/MatchEq.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Fft.h"
#include "synthmatch/match/Matcher.h"
#include "synthmatch/match/Modulation.h"
#include "synthmatch/match/Optimizer.h"
#include "synthmatch/match/Pitch.h"
#include "synthmatch/match/SearchSpace.h"
#include "synthmatch/match/Train.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
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

double rmsDb(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
        sum += static_cast<double>(v) * v;
    return 10.0 * std::log10(sum / static_cast<double>(std::max<std::size_t>(1, x.size())) + 1e-30);
}

// The part of a recording from `start` on, as a new recording.
StereoBuffer cropped(const StereoBuffer& audio, std::size_t start, std::size_t count = std::size_t(-1))
{
    StereoBuffer out;
    out.sampleRate = audio.sampleRate;
    const std::size_t end = std::min(audio.numFrames(), count == std::size_t(-1) ? audio.numFrames() : start + count);
    out.left.assign(audio.left.begin() + static_cast<std::ptrdiff_t>(start), audio.left.begin() + static_cast<std::ptrdiff_t>(end));
    out.right.assign(audio.right.begin() + static_cast<std::ptrdiff_t>(start), audio.right.begin() + static_cast<std::ptrdiff_t>(end));
    return out;
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
    p[param::ReverbOn] = 0.0f;
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

// ---------------------------------------------------------------- modulation

// A held saw on A3 with a gentle filter, for the vibrato and tremolo tests.
ParamValues heldSaw()
{
    auto p = defaultValues();
    p[param::Osc1Wave] = static_cast<float>(dsp::Wave::Saw);
    p[param::FilterCutoffHz] = 3000.0f;
    p[param::AmpReleaseMs] = 80.0f;
    return p;
}

ParamValues withLfo(ParamValues p, dsp::LfoTarget target, double rateHz, double depth,
                    dsp::LfoWave wave = dsp::LfoWave::Sine)
{
    p[param::Lfo1Target] = static_cast<float>(target);
    p[param::Lfo1Wave] = static_cast<float>(wave);
    p[param::Lfo1RateHz] = static_cast<float>(rateHz);
    p[param::Lfo1Depth] = static_cast<float>(depth);
    return p;
}

// Measures a recording's modulation the way the matcher does.
ModulationEstimate measure(const StereoBuffer& audio)
{
    const auto mono = toMono(audio.left, audio.right);
    const auto found = detectNotes(mono, audio.sampleRate);
    const std::vector<float> section(mono.begin() + static_cast<std::ptrdiff_t>(found.startSample),
                                     mono.begin() + static_cast<std::ptrdiff_t>(found.startSample + found.numSamples));
    SpectrogramConfig config;
    config.fftSize = 2048;
    config.hop = 512;
    const SpectrogramAnalyzer analyzer(audio.sampleRate, config);
    return measureModulation(found.pitch, analyzer.analyze(section), analyzer, found.notes);
}

void testModulation()
{
    // The depth conversions are the engine's: 12 * depth^2 semitones of
    // vibrato, and a level that swings between 1 and 1 - depth.
    CHECK_NEAR(vibratoDepth(27.0), 0.15, 1e-6);
    CHECK_NEAR(tremoloDepth(-10.0 * std::log10(0.7)), 0.3, 1e-6);

    const std::vector<NoteEvent> held{{0.05, 2.0, 57, 1.0f}};

    // Vibrato at 5.5 Hz, 27 cents.
    const auto vibrato = measure(renderNotes(withLfo(heldSaw(), dsp::LfoTarget::Pitch, 5.5, vibratoDepth(27.0)), held, 0.3));
    CHECK(vibrato.vibrato.found);
    CHECK(!vibrato.tremolo.found);
    CHECK_NEAR(vibrato.vibrato.rateHz, 5.5, 0.02 * 5.5);
    CHECK_NEAR(vibrato.vibrato.cents, 27.0, 2.7);
    CHECK_NEAR(vibrato.rateHz(), vibrato.vibrato.rateHz, 1e-12);

    // Tremolo at 6 Hz, depth 0.3.
    const auto tremolo = measure(renderNotes(withLfo(heldSaw(), dsp::LfoTarget::Amp, 6.0, 0.3), held, 0.3));
    CHECK(!tremolo.vibrato.found);
    CHECK(tremolo.tremolo.found);
    CHECK_NEAR(tremolo.tremolo.rateHz, 6.0, 0.12);
    CHECK_NEAR(tremoloDepth(tremolo.tremolo.db), 0.3, 0.05);

    // Look-alikes: beating between detuned copies, and a moving cutoff. They
    // wobble the level, and a little the pitch, but they aren't vibrato or tremolo.
    auto unison = heldSaw();
    unison[param::UnisonVoices] = 7.0f;
    unison[param::UnisonDetune] = 35.0f;
    auto pair = heldSaw();
    pair[param::Osc2Level] = 1.0f;
    pair[param::Osc2Fine] = 7.0f;
    auto cutoff = withLfo(heldSaw(), dsp::LfoTarget::Cutoff, 2.0, 0.3);
    cutoff[param::FilterCutoffHz] = 1000.0f;
    const std::vector<NoteEvent> high{{0.05, 2.0, 81, 1.0f}};
    for (const auto& [params, notes] : {std::pair{unison, high}, std::pair{pair, high}, std::pair{cutoff, held},
                                        std::pair{heldSaw(), held}}) {
        const auto m = measure(renderNotes(params, notes, 0.3));
        CHECK(!m.vibrato.found);
        CHECK(!m.tremolo.found);
        CHECK(!m.found());
    }
}

void testModulationDistance()
{
    // A vibrato recorded half a cycle into its swing: the synth's LFO always
    // starts at the top of the note, so it can never line up with this one.
    const double rate = 5.5;
    const auto vibrato = withLfo(heldSaw(), dsp::LfoTarget::Pitch, rate, vibratoDepth(27.0));
    const auto full = renderNotes(vibrato, {{0.0, 2.2, 57, 1.0f}}, 0.3);
    const auto start = static_cast<std::size_t>(0.5 / rate * full.sampleRate);
    const auto reference = cropped(full, start);
    const std::vector<NoteEvent> notes{{0.0, 2.2 - 0.5 / rate, 57, 1.0f}};

    // The right vibrato still scores closer than none, and closer than a wrong rate.
    Preset right, none, fast;
    right.params = vibrato;
    none.params = heldSaw();
    fast.params = withLfo(heldSaw(), dsp::LfoTarget::Pitch, rate * 1.1, vibratoDepth(27.0));
    const double rightDistance = presetDistance(reference, right, notes);
    const double noneDistance = presetDistance(reference, none, notes);
    const double fastDistance = presetDistance(reference, fast, notes);
    std::printf("vibrato half a cycle out: right %.2f, none %.2f, 10%% fast %.2f\n", rightDistance, noneDistance, fastDistance);
    CHECK(rightDistance < noneDistance);
    CHECK(rightDistance < fastDistance);

    // A steady sound is measured as it always was: no modulation, no profile,
    // and the plain spectral distance at the level the preset plays at.
    const ModulationProfile inactive;
    CHECK(!inactive.active());
    {
        const std::vector<NoteEvent> held{{0.0, 0.8, 57, 1.0f}};
        const auto steady = renderNotes(heldSaw(), held, 0.3);
        CHECK(!measure(steady).found());
        Preset darker;
        darker.params = heldSaw();
        darker.params[param::FilterCutoffHz] = 1200.0f;
        RenderSettings settings;
        settings.sampleRate = steady.sampleRate;
        settings.tailSec = static_cast<double>(steady.numFrames()) / steady.sampleRate - 0.8;
        const auto played = render(darker, held, settings);
        SpectrogramConfig config;
        config.fftSize = 2048;
        config.hop = 512;
        const SpectrogramAnalyzer analyzer(steady.sampleRate, config);
        const double plain = spectralDistance(analyzer.analyze(toMono(steady.left, steady.right)),
                                              analyzer.analyze(toMono(played.left, played.right)));
        CHECK(plain > 0.5);
        CHECK_NEAR(presetDistance(steady, darker, held), plain, 1e-9);
    }
}

void testMatchFindsVibrato()
{
    // The same out-of-phase vibrato, matched from scratch.
    const double rate = 5.5;
    const auto vibrato = withLfo(heldSaw(), dsp::LfoTarget::Pitch, rate, vibratoDepth(27.0));
    const auto full = renderNotes(vibrato, {{0.0, 2.2, 57, 1.0f}}, 0.3);
    const auto reference = cropped(full, static_cast<std::size_t>(0.5 / rate * full.sampleRate));

    MatchOptions options;
    options.budget = 3000;
    options.threads = 2;
    const auto result = matchSound(reference, options);
    CHECK(result.ok);
    if (!result.ok)
        return;
    const auto& p = result.preset.params;
    std::printf("vibrato match: distance %.2f dB, lfo1 %s at %.2f Hz, depth %.3f\n", result.distance,
                std::string(choiceName(spec(param::Lfo1Target), p[param::Lfo1Target])).c_str(), p[param::Lfo1RateHz],
                p[param::Lfo1Depth]);
    CHECK(p[param::Lfo1Target] == static_cast<float>(dsp::LfoTarget::Pitch));
    CHECK_NEAR(p[param::Lfo1RateHz], rate, 0.05 * rate);
}

// ---------------------------------------------------------------- the key

void testMoveKey()
{
    // A key-tracked filter and a second oscillator two octaves up.
    auto p = defaultValues();
    p[param::FilterCutoffHz] = 800.0f;
    p[param::FilterKeyTrack] = 0.6f;
    p[param::FilterResonance] = 0.3f;
    p[param::Osc2Level] = 0.7f;
    p[param::Osc2Wave] = static_cast<float>(dsp::Wave::Square);
    p[param::Osc2Coarse] = 24.0f;

    auto moved = p;
    CHECK(moveKey(moved, 24));
    CHECK(moved[param::Osc1Coarse] == -24.0f);
    CHECK(moved[param::Osc2Coarse] == 0.0f);
    CHECK(moved[param::FilterCutoffHz] < p[param::FilterCutoffHz]);

    // Played two octaves higher, it sounds exactly the same.
    const auto before = renderNotes(p, {{0.0, 0.5, 33, 1.0f}}, 0.2);
    const auto after = renderNotes(moved, {{0.0, 0.5, 57, 1.0f}}, 0.2);
    CHECK(before.numFrames() == after.numFrames());
    double largest = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < before.numFrames(); ++i) {
        largest = std::max(largest, static_cast<double>(std::abs(before.left[i] - after.left[i])));
        peak = std::max(peak, static_cast<double>(std::abs(before.left[i])));
    }
    CHECK(peak > 0.05);
    CHECK(largest < 1e-5);

    // Moving by nothing changes nothing.
    auto same = p;
    CHECK(moveKey(same, 0) && same == p);

    // An audible oscillator can't be moved out of its range; nothing changes.
    auto low = p;
    low[param::Osc2Coarse] = -24.0f;
    auto refused = low;
    CHECK(!moveKey(refused, 24));
    CHECK(refused == low);
    // A silent one can (it is just kept in range).
    low[param::Osc2Level] = 0.0f;
    CHECK(moveKey(low, 24));
    CHECK(low[param::Osc2Coarse] == spec(param::Osc2Coarse).min);
}

void testMatchMovesKey()
{
    // A saw on A2 with a quiet sine two octaves below: the whole waveform
    // only repeats at the sub's period, so the note detected is A0. The
    // preset is moved so its saw (the loudest part) plays A2.
    auto p = defaultValues();
    p[param::Osc1Wave] = static_cast<float>(dsp::Wave::Saw);
    p[param::Osc1Level] = 1.0f;
    p[param::Osc2Wave] = static_cast<float>(dsp::Wave::Sine);
    p[param::Osc2Level] = 0.5f;
    p[param::Osc2Coarse] = -24.0f;
    p[param::AmpReleaseMs] = 100.0f;
    const auto reference = renderNotes(p, {{0.05, 0.8, 45, 1.0f}}, 0.5);
    const auto detected = detectNotes(toMono(reference.left, reference.right), reference.sampleRate);
    CHECK(detected.notes.size() == 1 && detected.notes.front().note == 21);

    MatchOptions options;
    options.budget = 3000;
    options.threads = 2;
    const auto result = matchSound(reference, options);
    CHECK(result.ok);
    if (!result.ok)
        return;
    std::printf("key match: notes moved %+d to %d, distance %.2f dB\n", result.keyShift, result.notes.front().note,
                result.distance);
    CHECK(result.notes.size() == 1 && result.notes.front().note == 45);
    CHECK(result.keyShift == 24);
    // The moved preset on the moved notes is the sound that was matched.
    CHECK_NEAR(presetDistance(cropped(reference, result.startSample, result.numSamples), result.preset, result.notes),
               result.distance, 0.01);
}

// ---------------------------------------------------------------- clean-up

void testSimplify()
{
    auto p = heldSaw();
    p[param::GainDb] = -6.0f;
    const std::vector<NoteEvent> notes{{0.0, 0.8, 45, 1.0f}};
    const auto reference = renderNotes(p, notes, 0.4);

    // An echo two seconds later, beyond the recording, with the mix up so the
    // dry sound is quieter and the gain up to make up for it: the same sound
    // in the recording, but loud echoes afterwards.
    Preset echo;
    echo.params = p;
    echo.params[param::DelayOn] = 1.0f;
    echo.params[param::DelayTimeMs] = 2000.0f;
    echo.params[param::DelayMix] = 0.9f;
    echo.params[param::GainDb] = p[param::GainDb] - static_cast<float>(20.0 * std::log10(2.0 * (1.0 - 0.9)));
    CHECK(presetDistance(reference, echo, notes) < 0.2);
    const auto plain = simplifyPreset(reference, echo, notes);
    CHECK(plain.params[param::DelayOn] == 0.0f);
    CHECK_NEAR(plain.params[param::GainDb], p[param::GainDb], 0.3);
    CHECK(presetDistance(reference, plain, notes) < 0.2);

    // A square pitch LFO too shallow to hear goes.
    Preset shallow;
    shallow.params = withLfo(p, dsp::LfoTarget::Pitch, 0.4, 0.04, dsp::LfoWave::Square);
    CHECK(simplifyPreset(reference, shallow, notes).params[param::Lfo1Target] == 0.0f);

    // But one the recording needs stays: here a slow square wave that holds
    // the note a semitone up for as long as it plays.
    const auto transposed = withLfo(p, dsp::LfoTarget::Pitch, 0.3, 0.3, dsp::LfoWave::Square);
    const auto bent = renderNotes(transposed, notes, 0.4);
    Preset needed;
    needed.params = transposed;
    const auto kept = simplifyPreset(bent, needed, notes);
    CHECK(kept.params == needed.params);
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
    p[param::ReverbOn] = 0.0f;
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

    // The search starts dry, although the default patch has a touch of reverb,
    // and can choose the reverb's mode.
    CHECK(space.base[param::ReverbOn] == 0.0f);
    CHECK(defaultValues()[param::ReverbOn] == 1.0f);
    std::size_t modes = 0;
    for (const auto& dim : space.dims)
        if (dim.param == param::ReverbMode) {
            CHECK(dim.extended);
            CHECK(dim.choices.size() == 5);
            ++modes;
        }
    CHECK(modes == 1);

    // What can be heard: a silent oscillator's settings and a switched-off effect's can't.
    auto p = space.base;
    p[param::Osc2Level] = 0.0f;
    p[param::ReverbOn] = 0.0f;
    p[param::DelayOn] = 1.0f;
    const auto audible = audibleDims(space, p);
    for (std::size_t i = 0; i < space.dims.size(); ++i) {
        const int index = space.dims[i].param;
        if (index == param::Osc2Wave || index == param::Osc2Coarse || index == param::ReverbDecaySec ||
            index == param::ReverbMode || index == param::Osc1PulseWidth)
            CHECK(!audible[i]);
        if (index == param::Osc2Level || index == param::Osc1Wave || index == param::DelayTimeMs ||
            index == param::FilterCutoffHz || index == param::ReverbOn)
            CHECK(audible[i]);
    }

    // A pitch LFO is heard from about 3 cents (depth 0.05).
    const auto lfoRateHeard = [&](float depth) {
        auto q = space.base;
        q[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Pitch);
        q[param::Lfo1Depth] = depth;
        const auto heard = audibleDims(space, q);
        for (std::size_t i = 0; i < space.dims.size(); ++i)
            if (space.dims[i].param == param::Lfo1RateHz)
                return static_cast<bool>(heard[i]);
        return false;
    };
    CHECK(!lfoRateHeard(0.04f));
    CHECK(lfoRateHeard(0.06f));

    // The LFO rate is searched over vibrato and tremolo speeds, not the
    // parameter's whole range.
    for (const auto& dim : space.dims) {
        if (dim.param == param::Lfo1RateHz) {
            CHECK_NEAR(dim.toValue(0.0), 0.5, 1e-4);
            CHECK_NEAR(dim.toValue(1.0), 15.0, 1e-3);
        }
    }
}

// ---------------------------------------------------------------- matching

void testMatchRecoversSound()
{
    // A square-wave bass with a filter sweep. The matcher only hears the audio.
    auto p = defaultValues();
    p[param::ReverbOn] = 0.0f;
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

    // The note detected, and the key the preset plays it from (whole octaves away at most).
    CHECK(result.notes.size() == 1 && result.notes[0].note - result.keyShift == 45);
    CHECK(result.keyShift % 12 == 0);
    CHECK(result.distance < 0.6 * result.startDistance);
    CHECK(result.distance < 4.0);
    CHECK(result.evaluations <= options.budget + 200);
    CHECK(result.score() > 60.0);

    // The broad character is right.
    const auto& found = result.preset.params;
    CHECK(found[param::Osc1Wave] == static_cast<float>(dsp::Wave::Square));
    CHECK(found[param::FilterType] == 0.0f);
    CHECK(found[param::FilterCutoffHz] < 4000.0f);

    // And the level: the preset plays as loud as the recording. Not GainDb
    // itself: the oscillator levels, an effect's dry level and the amp
    // envelope all trade against it, and the search may land on any of them.
    {
        double lastOff = 0.0;
        for (const auto& n : result.notes)
            lastOff = std::max(lastOff, n.startSec + n.durationSec);
        RenderSettings settings;
        settings.sampleRate = reference.sampleRate;
        settings.tailSec = std::max(0.0, static_cast<double>(result.numSamples) / reference.sampleRate - lastOff);
        const auto played = render(result.preset, result.notes, settings);
        const auto mono = toMono(reference.left, reference.right);
        const std::span<const float> section(mono.data() + result.startSample, result.numSamples);
        CHECK_NEAR(rmsDb(toMono(played.left, played.right)), rmsDb(section), 1.5);
        CHECK(found[param::GainDb] > spec(param::GainDb).min && found[param::GainDb] < spec(param::GainDb).max);
    }

    // The reported distance is what an independent check measures.
    CHECK_NEAR(presetDistance(cropped(reference, result.startSample, result.numSamples), result.preset, result.notes),
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
    coloured.params[param::ReverbOn] = 0.0f;
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

    // Random sounds stay inside every parameter's range, and differ. Their
    // reverbs come in every mode.
    std::set<float> reverbModes;
    int vibratos = 0;
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        const auto sound = randomSound(space, seed);
        for (int i = 0; i < kNumParams; ++i)
            CHECK(sound.params[static_cast<std::size_t>(i)] == clampToRange(spec(i), sound.params[static_cast<std::size_t>(i)]));
        CHECK(sound.note.note >= 28 && sound.note.note <= 88);
        reverbModes.insert(sound.params[param::ReverbMode]);
        // The LFO stays within the search's rates. A vibrato is audible (5 to
        // 80 cents), and its note long enough to hear a few cycles.
        const auto target = static_cast<dsp::LfoTarget>(static_cast<int>(sound.params[param::Lfo1Target]));
        if (target != dsp::LfoTarget::Off) {
            CHECK(sound.params[param::Lfo1RateHz] >= 0.5f && sound.params[param::Lfo1RateHz] <= 15.0f);
        }
        if (target == dsp::LfoTarget::Pitch) {
            ++vibratos;
            const double cents = 1200.0 * sound.params[param::Lfo1Depth] * sound.params[param::Lfo1Depth];
            CHECK(cents >= 4.99 && cents <= 80.01);
            CHECK(sound.note.durationSec >= 0.7);
        }
    }
    CHECK(reverbModes.size() == 5);
    CHECK(vibratos > 15 && vibratos < 60);
    CHECK(randomSound(space, 3).params != randomSound(space, 4).params);

    // Six measurements of vibrato and tremolo come last.
    CHECK(encoderFeatureCount() == 631);
    const auto vibratoNote = renderNotes(withLfo(heldSaw(), dsp::LfoTarget::Pitch, 5.5, vibratoDepth(27.0)),
                                         {{0.0, 1.5, 57, 1.0f}}, 0.3, 24000.0);
    const auto plainNote = renderNotes(heldSaw(), {{0.0, 1.5, 57, 1.0f}}, 0.3, 24000.0);
    const NoteEvent note{0.0, 1.5, 57, 1.0f};
    const auto vib = encoderFeatures(toMono(vibratoNote.left, vibratoNote.right), 24000.0, note);
    const auto steady = encoderFeatures(toMono(plainNote.left, plainNote.right), 24000.0, note);
    const std::size_t m = encoderFeatureCount() - 6;
    CHECK_NEAR(vib[m + 1], 27.0 / 50.0, 0.06); // depth in cents / 50
    CHECK(vib[m + 2] > 0.7f);                  // a steady wobble
    CHECK(steady[m + 1] < 0.05f);
    for (const float v : vib)
        CHECK(v >= 0.0f && v <= 1.0f);

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

    // A name of its own, so test runs side by side don't share the file.
    const auto path = std::filesystem::temp_directory_path() /
                      ("synthmatch_test_model_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".smm");
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
    // Detecting notes and augmenting (both on by default) keep training
    // deterministic, and switching them off still trains.
    TrainOptions quick = options;
    quick.samples = 300;
    quick.epochs = 1;
    quick.log = nullptr;
    const auto first = trainEncoder(quick), second = trainEncoder(quick);
    CHECK(first.ok && second.ok && first.report.validationLoss == second.report.validationLoss);
    TrainOptions plain = quick;
    plain.detectNotes = false;
    plain.augment = false;
    const auto clean = trainEncoder(plain);
    CHECK(clean.ok && clean.report.trainLoss != first.report.trainLoss);

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

    // The file records each dimension's range: a model trained for another
    // LFO rate range is refused rather than used with the wrong scale.
    {
        std::size_t at = 16; // magic, version, feature count, dimension count
        std::size_t rateLo = 0;
        for (const auto& dim : space.dims) {
            at += 4 + spec(dim.param).id.size() + 4; // the id, then the number of choices
            if (dim.param == param::Lfo1RateHz)
                rateLo = at;
            at += 4 + 4 + 4; // lo, hi, extended
        }
        CHECK(rateLo > 0);
        auto changed = bytes;
        float lo = 0.0f;
        std::memcpy(&lo, changed.data() + rateLo, sizeof lo);
        CHECK(lo == 0.5f);
        lo = 0.05f;
        std::memcpy(changed.data() + rateLo, &lo, sizeof lo);
        error.clear();
        CHECK(!Encoder::fromMemory(changed, &error));
        CHECK(error.find("Train it again") != std::string::npos);
    }
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
    testModulation();
    testModulationDistance();
    testMoveKey();
    testSimplify();
    testMatchEqFit();
    testEigen();
    testCmaEs();
    testSearchSpace();
    testMatchRecoversSound();
    testMatchMovesKey();
    testMatchFindsVibrato();
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
