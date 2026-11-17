// Engine tests. Plain asserts, no framework; run through CTest.
//
// Golden renders live in tests/golden. After an intentional change to the
// sound, regenerate them with:  synthmatch_engine_tests --update-golden

#include "synthmatch/Engine.h"
#include "synthmatch/Params.h"
#include "synthmatch/Json.h"
#include "synthmatch/Preset.h"
#include "synthmatch/Render.h"
#include "synthmatch/dsp/Envelope.h"
#include "synthmatch/dsp/Biquad.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/MatchEq.h"
#include "synthmatch/dsp/Oscillator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef SYNTHMATCH_GOLDEN_DIR
#error "SYNTHMATCH_GOLDEN_DIR must be defined by the build"
#endif

static_assert(std::endian::native == std::endian::little, "golden files are little-endian float32");

namespace {

int g_failures = 0;
bool g_updateGolden = false;

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
using dsp::FilterType;
using dsp::Wave;

constexpr double kPi = std::numbers::pi;

// ---------------------------------------------------------------- helpers

std::size_t at(double sec, double sampleRate) { return static_cast<std::size_t>(sec * sampleRate); }

float peak(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    float p = 0.0f;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i)
        p = std::max(p, std::abs(x[i]));
    return p;
}

// Frequency from rising zero crossings, interpolated between samples.
double estimateHz(const std::vector<float>& x, double sampleRate, std::size_t from, std::size_t to)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = from + 1; i < std::min(to, x.size()); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 2)
        return 0.0;
    return (crossings - 1) * sampleRate / (last - first);
}

// Amplitude of the sine component at the given frequency (in cycles per
// sample). Exact when x holds a whole number of cycles of it.
double amplitudeAt(const std::vector<double>& x, double cyclesPerSample)
{
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double a = 2.0 * kPi * cyclesPerSample * static_cast<double>(i);
        re += x[i] * std::cos(a);
        im -= x[i] * std::sin(a);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

double toDb(double ratio) { return 10.0 * std::log10(std::max(ratio, 1.0e-30)); }

std::vector<double> runOsc(Wave wave, double dt, std::size_t n, double pulseWidth = 0.5)
{
    std::vector<double> x(n);
    double phase = 0.0;
    for (auto& s : x) {
        s = dsp::oscSample(wave, phase, dt, pulseWidth);
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return x;
}

// The default patch without its touch of reverb.
ParamValues dryDefaults()
{
    auto p = defaultValues();
    p[param::ReverbOn] = 0.0f;
    return p;
}

// A patch with only oscillator 1, as a clean starting point for tests.
ParamValues sinePatch()
{
    auto p = dryDefaults();
    p[param::GainDb] = 0.0f;
    p[param::AmpAttackMs] = 1.0f;
    p[param::Osc1Wave] = static_cast<float>(Wave::Sine);
    return p;
}

StereoBuffer renderNote(const ParamValues& params, int note, double length, double tail, double sampleRate = 48000.0,
                        float velocity = 1.0f)
{
    const NoteEvent events[] = {{0.0, length, note, velocity}};
    RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.tailSec = tail;
    return render(params, events, settings);
}

// A bright, steady patch for filter tests: one saw at full level.
ParamValues sawPatch()
{
    auto p = dryDefaults();
    p[param::GainDb] = 0.0f;
    p[param::AmpAttackMs] = 1.0f;
    return p;
}

// The gain the filter should have at a frequency: the textbook two-pole
// response, with the frequency axis warped the way a digital filter warps it.
double svfGain(FilterType type, double sampleRate, double cutoffHz, double damping, double hz)
{
    const double w = std::tan(kPi * hz / sampleRate) / std::tan(kPi * cutoffHz / sampleRate);
    const double den = std::hypot(1.0 - w * w, damping * w);
    switch (type) {
    case FilterType::BandPass:
        return w / den;
    case FilterType::HighPass:
        return w * w / den;
    default:
        return 1.0 / den;
    }
}

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// Renders a held saw note and compares its first 10 harmonics with the raw
// oscillator's harmonics times the filter gain expected for the given cutoff.
// Returns the worst relative error. The note must be an A (110, 220, 440 or
// 880 Hz) so that half a second holds a whole number of cycles.
double filteredSawError(const ParamValues& p, int note, double expectedCutoffHz)
{
    const double sr = 48000.0;
    const double hz = noteHz(note);
    const auto audio = renderNote(p, note, 1.0, 0.0, sr);
    const std::vector<double> steady(audio.left.begin() + static_cast<std::ptrdiff_t>(at(0.5, sr)),
                                     audio.left.begin() + static_cast<std::ptrdiff_t>(at(1.0, sr)));

    // The oscillator as the engine plays it: at twice the rate, filtered back
    // down (skipping the decimator's start-up).
    const std::size_t warm = dsp::kDecimatorTaps;
    const auto fast = runOsc(Wave::Saw, hz / (2.0 * sr), 2 * (steady.size() + warm));
    dsp::Decimator decimator;
    std::vector<double> raw;
    for (std::size_t i = 0; i < steady.size() + warm; ++i) {
        const double y = decimator.process(fast[2 * i], fast[2 * i + 1], dsp::decimatorTaps());
        if (i >= warm)
            raw.push_back(y);
    }

    const auto type = static_cast<FilterType>(static_cast<int>(p[param::FilterType]));
    const double damping = dsp::resonanceToDamping(p[param::FilterResonance]);

    double worst = 0.0;
    for (int h = 1; h <= 10; ++h) {
        const double f = h * hz / sr;
        const double expected = amplitudeAt(raw, f) * svfGain(type, sr, expectedCutoffHz, damping, h * hz);
        worst = std::max(worst, std::abs(amplitudeAt(steady, f) - expected) / (expected + 1.0e-3));
    }
    return worst;
}

// Amplitude of one frequency inside a time window of a render.
double harmonicIn(const StereoBuffer& audio, double fromSec, double toSec, double hz)
{
    const std::vector<double> window(audio.left.begin() + static_cast<std::ptrdiff_t>(at(fromSec, audio.sampleRate)),
                                     audio.left.begin() + static_cast<std::ptrdiff_t>(at(toSec, audio.sampleRate)));
    return amplitudeAt(window, hz / audio.sampleRate);
}

// ---------------------------------------------------------------- schema

void testSchema()
{
    std::set<std::string_view> ids;
    for (const auto& s : schema()) {
        CHECK(!s.id.empty());
        CHECK(ids.insert(s.id).second); // ids are unique
        CHECK(s.min < s.max);
        CHECK(s.defaultValue >= s.min && s.defaultValue <= s.max);
        if (s.curve == Curve::Log)
            CHECK(s.min > 0.0f);
        if (!s.choices.empty()) {
            CHECK(s.curve == Curve::Stepped);
            CHECK(static_cast<float>(s.choices.size()) == s.max - s.min + 1.0f);
        }

        // value -> normalized -> value survives the round trip.
        for (const float t : {0.0f, 0.25f, 0.5f, 1.0f}) {
            const float v = fromNormalized(s, t);
            CHECK(v >= s.min && v <= s.max);
            CHECK_NEAR(fromNormalized(s, toNormalized(s, v)), v, 1e-3 * (s.max - s.min));
        }
        CHECK_NEAR(fromNormalized(s, 0.0f), s.min, 1e-6);
        CHECK_NEAR(fromNormalized(s, 1.0f), s.max, 1e-3 * s.max);
    }

    CHECK(static_cast<int>(schema().size()) == kNumParams);
    CHECK(findParam("gain_db") == param::GainDb);
    CHECK(findParam("osc2_fine") == param::Osc2Fine);
    CHECK(findParam("osc3_pw") == param::osc(2, param::OscPulseWidth));
    CHECK(findParam("noise_level") == param::NoiseLevel);
    CHECK(findParam("filter_type") == param::FilterType);
    CHECK(findParam("filter_release_ms") == param::FilterReleaseMs);
    CHECK(findParam("matcheq_amount") == param::MatchEqAmount);
    CHECK(findParam("lfo2_depth") == param::lfo(1, param::LfoDepth));
    CHECK(kNumParams == 79);

    // A touch of hall reverb by default; the mode is a named choice.
    const auto defaults = defaultValues();
    CHECK(defaults[param::ReverbOn] == 1.0f);
    CHECK(defaults[param::ReverbMix] == 0.15f);
    CHECK(choiceName(spec(param::ReverbMode), defaults[param::ReverbMode]) == "hall");
    CHECK(parseChoice(spec(param::ReverbMode), "vintage") == 4.0f);
    CHECK(findParam("reverb_highcut_hz") == param::ReverbHighCutHz);
    CHECK(nodeOf(param::ReverbMode) == "reverb");
    CHECK(spec(param::UnisonDetune).max == 50.0f);

    // Every parameter belongs to a node, and node + field leads back to it.
    CHECK(nodeOf(param::FilterCutoffHz) == "filter");
    CHECK(fieldOf(param::FilterCutoffHz) == "cutoff_hz");
    CHECK(nodeOf(param::GainDb) == "master");
    CHECK(fieldOf(param::GainDb) == "gain_db");
    CHECK(nodeOf(param::AmpAttackMs) == "amp");
    CHECK(findParam("reverb", "mix") == param::ReverbMix);
    CHECK(!findParam("reverb", "banana"));
    for (int i = 0; i < kNumParams; ++i)
        CHECK(findParam(nodeOf(i), fieldOf(i)) == i);
    CHECK(!findParam("does_not_exist"));

    const auto& wave = spec(param::Osc1Wave);
    CHECK(choiceName(wave, 3.0f) == "square");
    CHECK(parseChoice(wave, "triangle") == 1.0f);
    CHECK(!parseChoice(wave, "banana"));
    CHECK(choiceName(spec(param::GainDb), 0.0f).empty());
    CHECK(choiceName(spec(param::FilterType), 2.0f) == "highpass");
    CHECK(parseChoice(spec(param::FilterType), "bandpass") == 1.0f);
}

// ---------------------------------------------------------------- oscillators

void testOscillatorHarmonics()
{
    // 100 Hz at 48 kHz: exactly 480 samples per cycle, 10 whole cycles.
    const double dt = 1.0 / 480.0;
    const std::size_t n = 4800;

    const auto saw = runOsc(Wave::Saw, dt, n);
    const auto square = runOsc(Wave::Square, dt, n);
    const auto triangle = runOsc(Wave::Triangle, dt, n);
    const auto pulse = runOsc(Wave::Square, dt, n, 0.25);
    const auto sine = runOsc(Wave::Sine, dt, n);

    for (int h = 1; h <= 10; ++h) {
        const double f = h * dt;
        const bool odd = h % 2 == 1;

        // Saw: every harmonic at 2/(pi h).
        CHECK_NEAR(amplitudeAt(saw, f), 2.0 / (kPi * h), 0.01 * 2.0 / (kPi * h));

        // Square: odd harmonics at 4/(pi h), no even ones.
        CHECK_NEAR(amplitudeAt(square, f), odd ? 4.0 / (kPi * h) : 0.0, odd ? 0.01 * 4.0 / (kPi * h) : 1e-4);

        // Triangle: odd harmonics at 8/(pi h)^2, no even ones.
        const double tri = 8.0 / (kPi * kPi * h * h);
        CHECK_NEAR(amplitudeAt(triangle, f), odd ? tri : 0.0, odd ? 0.01 * tri : 1e-4);

        // 25% pulse: 4/(pi h) * |sin(pi h / 4)|.
        const double pulseAmp = 4.0 / (kPi * h) * std::abs(std::sin(kPi * h * 0.25));
        CHECK_NEAR(amplitudeAt(pulse, f), pulseAmp, 0.01 * 4.0 / (kPi * h) + 1e-4);

        CHECK_NEAR(amplitudeAt(sine, f), h == 1 ? 1.0 : 0.0, 1e-6);
    }

    // No DC offset, including for the asymmetric pulse.
    for (const auto* x : {&saw, &square, &triangle, &pulse, &sine}) {
        double sum = 0.0;
        for (const double s : *x)
            sum += s;
        CHECK_NEAR(sum / static_cast<double>(n), 0.0, 1e-3);
    }

    // Sine, triangle and saw start at zero and rise.
    for (const Wave w : {Wave::Sine, Wave::Triangle, Wave::Saw}) {
        CHECK_NEAR(dsp::oscSample(w, 0.0, dt, 0.5), 0.0, 1e-9);
        CHECK(dsp::oscSample(w, dt, dt, 0.5) > 0.0);
    }
}

void testOscillatorAliasing()
{
    // A high note (about 3.5 kHz at 48 kHz) that repeats exactly every 4096
    // samples. Every true harmonic sits at a multiple of bin 301, so anything
    // below bin 301 is aliasing folded down from above half the sample rate.
    const std::size_t n = 4096;
    const int fundamentalBin = 301;
    const double dt = static_cast<double>(fundamentalBin) / static_cast<double>(n);

    const auto aliasDb = [&](const std::vector<double>& x) {
        double alias = 0.0;
        for (int bin = 1; bin < fundamentalBin; ++bin) {
            const double a = amplitudeAt(x, static_cast<double>(bin) / static_cast<double>(n));
            alias += a * a;
        }
        const double fundamental = amplitudeAt(x, dt);
        return toDb(alias / (fundamental * fundamental));
    };

    std::vector<double> naiveSaw(n), naiveSquare(n), naiveTriangle(n);
    {
        double phase = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            naiveSaw[i] = 2.0 * dsp::wrap01(phase + 0.5) - 1.0;
            naiveSquare[i] = phase < 0.5 ? 1.0 : -1.0;
            const double q = dsp::wrap01(phase + 0.25);
            naiveTriangle[i] = q < 0.5 ? 4.0 * q - 1.0 : 3.0 - 4.0 * q;
            phase = dsp::wrap01(phase + dt);
        }
    }

    const double saw = aliasDb(runOsc(Wave::Saw, dt, n));
    const double square = aliasDb(runOsc(Wave::Square, dt, n));
    const double triangle = aliasDb(runOsc(Wave::Triangle, dt, n));
    const double sawNaive = aliasDb(naiveSaw);
    const double squareNaive = aliasDb(naiveSquare);
    const double triangleNaive = aliasDb(naiveTriangle);

    std::printf("aliasing below the fundamental at 3.5 kHz (dB relative to it):\n"
                "  saw      %7.1f  (naive %7.1f)\n"
                "  square   %7.1f  (naive %7.1f)\n"
                "  triangle %7.1f  (naive %7.1f)\n",
                saw, sawNaive, square, squareNaive, triangle, triangleNaive);

    // Measured 2026-11: saw -74.8, square -75.4, triangle -97.7 (naive: -17.5,
    // -20.3, -44.3). The limits leave about 10 dB of margin.
    CHECK(saw < -65.0);
    CHECK(square < -65.0);
    CHECK(triangle < -85.0);
    CHECK(saw < sawNaive - 40.0);
    CHECK(square < squareNaive - 40.0);
    CHECK(triangle < triangleNaive - 40.0);
}

void testOversampling()
{
    // A saw at C7 (2093 Hz) through the whole engine, filter open. Its 12th to
    // 17th harmonics lie above half the sample rate and fold back to known
    // frequencies, each at least 100 Hz from a true harmonic, where a windowed
    // probe can measure them. Oversampling should cut them a lot.
    const double sr = 48000.0, f0 = 440.0 * std::pow(2.0, (96 - 69) / 12.0);
    const auto loudestAlias = [&](bool oversample) {
        auto p = sawPatch();
        p[param::FilterCutoffHz] = 20000.0f;
        const NoteEvent events[] = {{0.0, 0.8, 96, 1.0f}};
        RenderSettings settings;
        settings.sampleRate = sr;
        settings.tailSec = 0.0;
        settings.oversample = oversample;
        const auto audio = render(p, events, settings);

        // 0.5 s of the steady part, Blackman-Harris windowed.
        const std::size_t n = at(0.5, sr), from = at(0.2, sr);
        std::vector<double> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
            const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            x[i] = w * audio.left[from + i];
        }
        const double fundamental = amplitudeAt(x, f0 / sr);
        double worst = 0.0;
        for (int k = 12; k <= 17; ++k) {
            const double folded = std::abs(k * f0 - sr * std::round(k * f0 / sr));
            worst = std::max(worst, amplitudeAt(x, folded / sr));
        }
        return toDb((worst * worst) / (fundamental * fundamental));
    };

    const double plain = loudestAlias(false), oversampled = loudestAlias(true);
    std::printf("loudest alias of a saw at C7 (dB relative to the fundamental): %.1f, oversampled %.1f\n", plain,
                oversampled);
    // Measured 2026-11: -36.4 plain and -82.7 oversampled.
    CHECK(oversampled < -55.0);
    CHECK(oversampled < plain - 15.0);
}

// ---------------------------------------------------------------- envelope

void testAdsrShape()
{
    const double sr = 48000.0;
    const auto c = dsp::AdsrCoeffs::make(sr, 10.0, 100.0, 0.5, 200.0);
    dsp::AdsrState s;
    s.gateOn();

    std::vector<float> level;
    for (int i = 0; i < static_cast<int>(0.5 * sr); ++i)
        level.push_back(dsp::adsrTick(s, c));
    s.gateOff();
    for (int i = 0; i < static_cast<int>(0.5 * sr); ++i)
        level.push_back(dsp::adsrTick(s, c));

    CHECK_NEAR(level[at(0.005, sr)], 0.5, 0.01);  // halfway up the linear attack
    CHECK_NEAR(level[at(0.010, sr)], 1.0, 0.01);  // attack done at 10 ms
    CHECK_NEAR(level[at(0.110, sr)], 0.5 + 0.5 * 0.001, 0.001); // 60 dB of the way to sustain
    CHECK_NEAR(level[at(0.400, sr)], 0.5, 1e-4);  // holding sustain
    CHECK_NEAR(level[at(0.700, sr)], 0.5 * 0.001, 1e-4); // 60 dB down after the release time
    CHECK(level.back() == 0.0f);
    CHECK(!s.active());

    for (std::size_t i = 1; i < level.size(); ++i)
        CHECK(std::abs(level[i] - level[i - 1]) < 0.01f); // no jumps anywhere
}

// ---------------------------------------------------------------- engine

void testSilenceWithoutNotes()
{
    RenderSettings settings;
    settings.tailSec = 0.5;
    const auto audio = render(defaultValues(), {}, settings);
    CHECK(audio.numFrames() == at(0.5, 48000.0));
    CHECK(peak(audio.left, 0, audio.numFrames()) == 0.0f);
}

void testDeterminism()
{
    auto p = defaultValues();
    p[param::Osc2Level] = 0.7f;
    p[param::Osc2Wave] = static_cast<float>(Wave::Square);
    p[param::Osc2Fine] = 9.0f;
    p[param::NoiseLevel] = 0.3f;

    const auto a = renderNote(p, 57, 0.3, 0.3);
    const auto b = renderNote(p, 57, 0.3, 0.3);
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
    CHECK(peak(a.left, 0, a.numFrames()) > 0.1f);
}

void testPitch()
{
    for (const double sr : {44100.0, 48000.0, 96000.0}) {
        const auto a4 = renderNote(sinePatch(), 69, 1.0, 0.0, sr);
        CHECK_NEAR(estimateHz(a4.left, sr, at(0.1, sr), at(0.9, sr)), 440.0, 0.05);

        const auto c4 = renderNote(sinePatch(), 60, 1.0, 0.0, sr);
        CHECK_NEAR(estimateHz(c4.left, sr, at(0.1, sr), at(0.9, sr)), 261.6256, 0.05);
    }

    // Every waveform plays at the same pitch.
    for (const Wave w : {Wave::Triangle, Wave::Saw, Wave::Square}) {
        auto p = sinePatch();
        p[param::Osc1Wave] = static_cast<float>(w);
        const auto audio = renderNote(p, 69, 1.0, 0.0);
        CHECK_NEAR(estimateHz(audio.left, 48000.0, at(0.1, 48000.0), at(0.9, 48000.0)), 440.0, 0.05);
    }
}

void testTuning()
{
    const double sr = 48000.0;
    const auto hz = [&](const ParamValues& p) {
        const auto audio = renderNote(p, 69, 1.0, 0.0, sr);
        return estimateHz(audio.left, sr, at(0.1, sr), at(0.9, sr));
    };

    auto p = sinePatch();
    p[param::Osc1Coarse] = 12.0f;
    CHECK_NEAR(hz(p), 880.0, 0.1);

    p[param::Osc1Coarse] = -12.0f;
    CHECK_NEAR(hz(p), 220.0, 0.05);

    p[param::Osc1Coarse] = 0.0f;
    p[param::Osc1Fine] = 100.0f; // 100 cents = one semitone
    CHECK_NEAR(hz(p), 466.1638, 0.05);

    // Oscillator 3 alone, tuned up a fifth.
    p = sinePatch();
    p[param::Osc1Level] = 0.0f;
    p[param::Osc3Level] = 1.0f;
    p[param::Osc3Wave] = static_cast<float>(Wave::Sine);
    p[param::Osc3Coarse] = 7.0f;
    CHECK_NEAR(hz(p), 659.2551, 0.05);
}

void testGainAndVelocity()
{
    auto p = sinePatch();
    p[param::GainDb] = -6.0f;

    const double sr = 48000.0;
    const float expected = std::pow(10.0f, -6.0f / 20.0f);

    const auto full = renderNote(p, 69, 0.5, 0.0, sr, 1.0f);
    CHECK_NEAR(peak(full.left, at(0.1, sr), at(0.4, sr)), expected, 0.002);

    const auto half = renderNote(p, 69, 0.5, 0.0, sr, 0.5f);
    CHECK_NEAR(peak(half.left, at(0.1, sr), at(0.4, sr)), expected * 0.5, 0.002);

    p[param::Osc1Level] = 0.25f;
    const auto quiet = renderNote(p, 69, 0.5, 0.0, sr, 1.0f);
    CHECK_NEAR(peak(quiet.left, at(0.1, sr), at(0.4, sr)), expected * 0.25, 0.002);
}

void testMixer()
{
    const double sr = 48000.0;

    // Two sines an octave apart, measured separately in the mix.
    auto p = sinePatch();
    p[param::Osc1Level] = 0.6f;
    p[param::Osc2Wave] = static_cast<float>(Wave::Sine);
    p[param::Osc2Level] = 0.3f;
    p[param::Osc2Coarse] = 12.0f;

    const auto audio = renderNote(p, 69, 1.0, 0.0, sr);
    // 440 Hz and 880 Hz both fit a whole number of cycles in 0.5 s.
    const std::vector<double> steady(audio.left.begin() + static_cast<std::ptrdiff_t>(at(0.25, sr)),
                                     audio.left.begin() + static_cast<std::ptrdiff_t>(at(0.75, sr)));
    CHECK_NEAR(amplitudeAt(steady, 440.0 / sr), 0.6, 0.002);
    CHECK_NEAR(amplitudeAt(steady, 880.0 / sr), 0.3, 0.002);

    // All oscillators off: silence.
    p = sinePatch();
    p[param::Osc1Level] = 0.0f;
    const auto silent = renderNote(p, 69, 0.2, 0.1, sr);
    CHECK(peak(silent.left, 0, silent.numFrames()) == 0.0f);
}

void testNoise()
{
    auto p = sinePatch();
    p[param::Osc1Level] = 0.0f;
    p[param::NoiseLevel] = 0.5f;
    // Measure the noise source itself: a 20 Hz high-pass leaves all but the
    // lowest 0.1% of the band untouched, where the default 20 kHz low-pass
    // would remove the top of it.
    p[param::FilterType] = static_cast<float>(FilterType::HighPass);
    p[param::FilterCutoffHz] = 20.0f;

    const double sr = 48000.0;
    const auto audio = renderNote(p, 60, 1.0, 0.0, sr);

    double sum = 0.0, sumSq = 0.0;
    const std::size_t from = at(0.1, sr), to = at(0.9, sr);
    for (std::size_t i = from; i < to; ++i) {
        sum += audio.left[i];
        sumSq += static_cast<double>(audio.left[i]) * audio.left[i];
    }
    const double count = static_cast<double>(to - from);
    CHECK_NEAR(sum / count, 0.0, 0.01);
    // Uniform noise in [-0.5, 0.5] has an RMS of 0.5 / sqrt(3).
    CHECK_NEAR(std::sqrt(sumSq / count), 0.5 / std::sqrt(3.0), 0.01);
    // 0.5 from the source, plus the slow drift (about 0.01 RMS) the high-pass removes.
    CHECK(peak(audio.left, from, to) <= 0.56f);
}

void testEnvelopeInEngine()
{
    auto p = sinePatch();
    p[param::AmpAttackMs] = 10.0f;
    p[param::AmpDecayMs] = 100.0f;
    p[param::AmpSustain] = 0.5f;
    p[param::AmpReleaseMs] = 100.0f;

    const double sr = 48000.0;
    const auto audio = renderNote(p, 96, 0.5, 1.0, sr); // a high note, so peaks are closely spaced

    CHECK_NEAR(peak(audio.left, at(0.009, sr), at(0.012, sr)), 1.0, 0.03); // top of the attack
    CHECK_NEAR(peak(audio.left, at(0.3, sr), at(0.45, sr)), 0.5, 0.01);    // sustain
    CHECK(peak(audio.left, at(0.5 + 0.11, sr), audio.numFrames()) < 1.0e-3f); // released
    CHECK(peak(audio.left, at(0.5 + 0.2, sr), audio.numFrames()) == 0.0f);    // voice freed
}

void testSustainZeroFreesVoice()
{
    Engine engine;
    engine.prepare(48000.0);
    auto p = sinePatch();
    p[param::AmpDecayMs] = 50.0f;
    p[param::AmpSustain] = 0.0f;
    engine.setParams(p);

    engine.noteOn(60, 1.0f);
    CHECK(engine.activeVoiceCount() == 1);

    std::vector<float> left(4800), right(4800);
    for (int block = 0; block < 3; ++block) // 0.3 s with the key still held
        engine.process(left.data(), right.data(), 4800);
    CHECK(engine.activeVoiceCount() == 0);
}

void testVoiceStealing()
{
    Engine engine;
    engine.prepare(48000.0);
    for (int n = 40; n < 72; ++n)
        engine.noteOn(n, 0.5f);
    CHECK(engine.activeVoiceCount() == Engine::kMaxVoices);

    std::vector<float> left(512), right(512);
    engine.process(left.data(), right.data(), 512);
    for (const float s : left)
        CHECK(std::isfinite(s));

    engine.allSoundOff();
    CHECK(engine.activeVoiceCount() == 0);
}

void testExtremeNotes()
{
    // The highest and lowest notes with every waveform must stay finite and bounded.
    for (const int note : {0, 127}) {
        auto p = defaultValues();
        p[param::GainDb] = 0.0f;
        p[param::Osc1Coarse] = note == 127 ? 36.0f : -36.0f;
        p[param::Osc2Level] = 1.0f;
        p[param::Osc2Wave] = static_cast<float>(Wave::Square);
        p[param::Osc2PulseWidth] = 0.05f;
        p[param::Osc3Level] = 1.0f;
        p[param::Osc3Wave] = static_cast<float>(Wave::Triangle);

        const auto audio = renderNote(p, note, 0.2, 0.1);
        bool finite = true;
        for (const float s : audio.left)
            finite = finite && std::isfinite(s);
        CHECK(finite);
        CHECK(peak(audio.left, 0, audio.numFrames()) < 8.0f);
    }
}

// ---------------------------------------------------------------- filter

void testFilterResponse()
{
    const double sr = 48000.0;
    const double cutoff = 1000.0;

    // Resonance 0 is a flat (Butterworth) response; resonance 1 is the maximum Q.
    CHECK_NEAR(dsp::resonanceToDamping(0.0), std::sqrt(2.0), 1e-12);
    CHECK_NEAR(dsp::resonanceToDamping(1.0), 1.0 / dsp::kMaxFilterQ, 1e-12);

    const auto measure = [&](FilterType type, double damping, double hz) {
        const auto c = dsp::SvfCoeffs::make(sr, cutoff, damping);
        dsp::SvfState s;
        const std::size_t settle = 48000, n = 4800; // 0.1 s: whole cycles of every test tone
        std::vector<double> out(n);
        for (std::size_t i = 0; i < settle + n; ++i) {
            const double y = dsp::svfTick(s, c, type, std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
            if (i >= settle)
                out[i - settle] = y;
        }
        return amplitudeAt(out, hz / sr);
    };

    for (const FilterType type : {FilterType::LowPass, FilterType::BandPass, FilterType::HighPass}) {
        for (const double resonance : {0.0, 0.5, 0.9}) {
            const double k = dsp::resonanceToDamping(resonance);
            for (const double hz : {100.0, 500.0, 1000.0, 2000.0, 8000.0, 15000.0}) {
                const double expected = svfGain(type, sr, cutoff, k, hz);
                CHECK_NEAR(measure(type, k, hz), expected, 1e-6 * (1.0 + expected));
            }
            // Every type has a gain of Q at the cutoff.
            CHECK_NEAR(measure(type, k, cutoff), 1.0 / k, 1e-6 / k);
        }
    }

    // 12 dB per octave: two octaves past the cutoff is about 24 dB down.
    // (toDb takes a power ratio, so amplitudes are squared.)
    const double k = dsp::resonanceToDamping(0.0);
    const auto ampDb = [](double amplitude) { return toDb(amplitude * amplitude); };
    CHECK_NEAR(ampDb(measure(FilterType::LowPass, k, 4000.0)), -24.0, 1.0);
    CHECK_NEAR(ampDb(measure(FilterType::HighPass, k, 250.0)), -24.0, 1.0);
    CHECK_NEAR(ampDb(measure(FilterType::LowPass, k, cutoff)), -3.01, 0.01);

    // Out-of-range cutoffs are clamped rather than breaking the filter.
    CHECK_NEAR(dsp::clampCutoff(1.0, sr), dsp::kMinCutoffHz, 1e-12);
    CHECK_NEAR(dsp::clampCutoff(1.0e6, sr), dsp::kMaxCutoffHz, 1e-12);
    CHECK(dsp::clampCutoff(1.0e6, 22050.0) < 0.5 * 22050.0);
}

void testFilterInEngine()
{
    // The default patch has a fully open low-pass at 20 kHz.
    CHECK(filteredSawError(sawPatch(), 57, 20000.0) < 2e-3);

    for (const FilterType type : {FilterType::LowPass, FilterType::BandPass, FilterType::HighPass}) {
        for (const float resonance : {0.0f, 0.6f}) {
            auto p = sawPatch();
            p[param::FilterType] = static_cast<float>(type);
            p[param::FilterCutoffHz] = 1000.0f;
            p[param::FilterResonance] = resonance;
            CHECK(filteredSawError(p, 57, 1000.0) < 2e-3);
            CHECK(filteredSawError(p, 57, 1100.0) > 2e-2); // the check can tell a wrong cutoff
        }
    }
}

void testFilterKeyTracking()
{
    auto p = sawPatch();
    p[param::FilterCutoffHz] = 600.0f;
    p[param::FilterResonance] = 0.3f;

    // No tracking: the same cutoff for every note.
    CHECK(filteredSawError(p, 57, 600.0) < 2e-3);
    CHECK(filteredSawError(p, 81, 600.0) < 2e-3);

    // Full tracking: the cutoff moves with the note, centred on middle C (60).
    p[param::FilterKeyTrack] = 1.0f;
    CHECK(filteredSawError(p, 57, 600.0 * std::pow(2.0, -3.0 / 12.0)) < 2e-3);
    CHECK(filteredSawError(p, 81, 600.0 * std::pow(2.0, 21.0 / 12.0)) < 2e-3);

    // Half tracking: half as far.
    p[param::FilterKeyTrack] = 0.5f;
    CHECK(filteredSawError(p, 81, 600.0 * std::pow(2.0, 0.5 * 21.0 / 12.0)) < 2e-3);
}

void testFilterEnvelope()
{
    // Held at a sustain of 1, the envelope raises the cutoff by the full amount.
    auto p = sawPatch();
    p[param::FilterCutoffHz] = 250.0f;
    p[param::FilterEnvAmount] = 3.0f; // octaves
    p[param::FilterAttackMs] = 1.0f;
    p[param::FilterSustain] = 1.0f;
    CHECK(filteredSawError(p, 57, 2000.0) < 2e-3);

    // At a sustain of 0.5 it settles halfway (in octaves).
    p[param::FilterEnvAmount] = 4.0f;
    p[param::FilterDecayMs] = 50.0f;
    p[param::FilterSustain] = 0.5f;
    CHECK(filteredSawError(p, 57, 1000.0) < 2e-3);

    // A negative amount lowers the cutoff.
    p[param::FilterCutoffHz] = 4000.0f;
    p[param::FilterEnvAmount] = -2.0f;
    p[param::FilterSustain] = 1.0f;
    CHECK(filteredSawError(p, 57, 1000.0) < 2e-3);

    // A decaying envelope makes the note start bright and end dull. Harmonic 20
    // of a 110 Hz note is 2.2 kHz; windows are 0.1 s = 11 whole cycles.
    const double sr = 48000.0;
    auto sweep = sawPatch();
    sweep[param::FilterCutoffHz] = 150.0f;
    sweep[param::FilterEnvAmount] = 7.0f;
    sweep[param::FilterAttackMs] = 1.0f;
    sweep[param::FilterDecayMs] = 1000.0f; // time to fall 60 dB, so it is half-way down by 0.1 s
    sweep[param::FilterSustain] = 0.0f;
    const auto swept = renderNote(sweep, 45, 1.0, 0.0, sr);
    const double early = harmonicIn(swept, 0.01, 0.11, 2200.0);
    const double late = harmonicIn(swept, 0.5, 0.6, 2200.0);
    CHECK(early > 0.01);
    CHECK(early > 10.0 * late);

    // With no envelope amount the same note doesn't change over time.
    sweep[param::FilterEnvAmount] = 0.0f;
    const auto fixed = renderNote(sweep, 45, 1.0, 0.0, sr);
    CHECK_NEAR(harmonicIn(fixed, 0.1, 0.2, 110.0), harmonicIn(fixed, 0.5, 0.6, 110.0), 1e-3);

    // After the key is released the filter envelope falls, closing the filter
    // while the (slow) amp release is still sounding.
    auto rel = sawPatch();
    rel[param::FilterCutoffHz] = 200.0f;
    rel[param::FilterEnvAmount] = 4.0f;
    rel[param::FilterAttackMs] = 1.0f;
    rel[param::FilterSustain] = 1.0f;
    rel[param::FilterReleaseMs] = 20.0f;
    rel[param::AmpReleaseMs] = 2000.0f;
    const auto released = renderNote(rel, 45, 0.3, 0.7, sr);
    const double heldBrightness = harmonicIn(released, 0.1, 0.2, 1100.0) / harmonicIn(released, 0.1, 0.2, 110.0);
    const double releasedBrightness = harmonicIn(released, 0.6, 0.7, 1100.0) / harmonicIn(released, 0.6, 0.7, 110.0);
    CHECK(harmonicIn(released, 0.6, 0.7, 110.0) > 0.05); // still sounding
    CHECK(releasedBrightness < 0.2 * heldBrightness);
}

void testBlockSizeIndependence()
{
    // The plugin processes blocks of any size, so the output must not depend on it.
    auto p = defaultValues();
    p[param::Osc2Level] = 0.5f;
    p[param::Osc2Fine] = 7.0f;
    p[param::NoiseLevel] = 0.2f;
    p[param::FilterCutoffHz] = 300.0f;
    p[param::FilterResonance] = 0.7f;
    p[param::FilterKeyTrack] = 0.5f;
    p[param::FilterEnvAmount] = 4.0f;
    p[param::FilterDecayMs] = 120.0f;
    p[param::FilterSustain] = 0.2f;
    p[param::UnisonVoices] = 3.0f;
    p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Pitch);
    p[param::Lfo1Depth] = 0.2f;
    p[param::Lfo2Target] = static_cast<float>(dsp::LfoTarget::Cutoff);
    p[param::Lfo2Wave] = static_cast<float>(dsp::LfoWave::Random);
    p[param::Lfo2RateHz] = 20.0f;
    p[param::EqOn] = 1.0f;
    p[param::EqLowGainDb] = 3.0f;
    p[param::DelayOn] = 1.0f;
    p[param::DelayTimeMs] = 70.0f;
    p[param::DelayModDepth] = 0.5f;
    p[param::ReverbOn] = 1.0f;

    const NoteEvent notes[] = {{0.0, 0.2, 48, 0.9f}, {0.0501, 0.2, 55, 0.7f}, {0.13, 0.1, 64, 0.8f}};
    std::vector<float> reference, referenceRight;
    for (const int blockSize : {256, 1, 32, 1000}) {
        RenderSettings settings;
        settings.blockSize = blockSize;
        settings.tailSec = 0.2;
        const auto audio = render(p, notes, settings);
        if (reference.empty()) {
            reference = audio.left;
            referenceRight = audio.right;
            CHECK(peak(reference, 0, reference.size()) > 0.05f);
        }
        CHECK(audio.left == reference);
        CHECK(audio.right == referenceRight);
    }
}

void testFilterStability()
{
    // Every extreme at once: maximum resonance, cutoff and envelope amount at
    // both ends, full key tracking, the highest and lowest notes, noise.
    for (const double sr : {44100.0, 48000.0, 96000.0}) {
        for (const FilterType type : {FilterType::LowPass, FilterType::BandPass, FilterType::HighPass}) {
            for (const float amount : {-8.0f, 8.0f}) {
                for (const int note : {0, 127}) {
                    auto p = defaultValues();
                    p[param::GainDb] = 0.0f;
                    p[param::Osc2Level] = 1.0f;
                    p[param::Osc2Wave] = static_cast<float>(Wave::Square);
                    p[param::NoiseLevel] = 1.0f;
                    p[param::FilterType] = static_cast<float>(type);
                    p[param::FilterCutoffHz] = amount > 0.0f ? 20000.0f : 20.0f;
                    p[param::FilterResonance] = 1.0f;
                    p[param::FilterKeyTrack] = 1.0f;
                    p[param::FilterEnvAmount] = amount;
                    p[param::FilterAttackMs] = 0.1f;
                    p[param::FilterDecayMs] = 30.0f;
                    p[param::FilterSustain] = 0.0f;

                    const auto audio = renderNote(p, note, 0.15, 0.1, sr);
                    bool finite = true;
                    for (const float s : audio.left)
                        finite = finite && std::isfinite(s);
                    CHECK(finite);
                    CHECK(peak(audio.left, 0, audio.numFrames()) < 100.0f);
                }
            }
        }
    }
}

// ---------------------------------------------------------------- unison, LFOs, pitch bend

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    to = std::min(to, x.size());
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

std::vector<double> slice(const std::vector<float>& x, double fromSec, double toSec, double sampleRate)
{
    return std::vector<double>(x.begin() + static_cast<std::ptrdiff_t>(at(fromSec, sampleRate)),
                               x.begin() + static_cast<std::ptrdiff_t>(at(toSec, sampleRate)));
}

void testUnison()
{
    const double sr = 48000.0;
    const std::size_t from = at(0.2, sr), to = at(1.0, sr);
    const auto power = [&](const StereoBuffer& a) {
        const double l = rms(a.left, from, to), r = rms(a.right, from, to);
        return l * l + r * r;
    };

    // One voice: both channels are identical.
    auto p = sawPatch();
    const auto mono = renderNote(p, 57, 1.0, 0.0, sr);
    CHECK(mono.left == mono.right);

    // Five detuned copies spread across the stereo field.
    p[param::UnisonVoices] = 5.0f;
    p[param::UnisonDetune] = 30.0f;
    p[param::UnisonSpread] = 1.0f;
    const auto wide = renderNote(p, 57, 1.0, 0.0, sr);
    CHECK(wide.left != wide.right);
    CHECK_NEAR(toDb(power(wide) / power(mono)), 0.0, 2.0); // about as loud as one voice

    // No spread: still mono, however many copies.
    p[param::UnisonSpread] = 0.0f;
    const auto narrow = renderNote(p, 57, 1.0, 0.0, sr);
    CHECK(narrow.left == narrow.right);

    // Two sine copies, 50 cents (the most detune) either side of the note.
    auto s = sinePatch();
    s[param::UnisonVoices] = 2.0f;
    s[param::UnisonDetune] = 50.0f;
    s[param::UnisonSpread] = 0.0f;
    const auto pair = renderNote(s, 69, 1.5, 0.0, sr);
    const auto steady = slice(pair.left, 0.25, 1.25, sr);
    CHECK_NEAR(amplitudeAt(steady, 440.0 * std::pow(2.0, -50.0 / 1200.0) / sr), std::sqrt(0.5), 0.02);
    CHECK_NEAR(amplitudeAt(steady, 440.0 * std::pow(2.0, 50.0 / 1200.0) / sr), std::sqrt(0.5), 0.02);
    CHECK(amplitudeAt(steady, 440.0 / sr) < 0.02);
    // More than the most is held at the most.
    s[param::UnisonDetune] = 100.0f;
    CHECK(renderNote(s, 69, 1.5, 0.0, sr).left == pair.left);
}

void testLfo()
{
    const double sr = 48000.0;

    // Vibrato: depth 0.5 is +/- 12 * 0.25 = 3 semitones. A 5 Hz sine peaks at
    // 0.05 s and bottoms out at 0.15 s.
    auto p = sinePatch();
    p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Pitch);
    p[param::Lfo1RateHz] = 5.0f;
    p[param::Lfo1Depth] = 0.5f;
    const auto vibrato = renderNote(p, 69, 0.5, 0.0, sr);
    CHECK_NEAR(estimateHz(vibrato.left, sr, at(0.045, sr), at(0.055, sr)), 440.0 * std::pow(2.0, 3.0 / 12.0), 8.0);
    CHECK_NEAR(estimateHz(vibrato.left, sr, at(0.145, sr), at(0.155, sr)), 440.0 * std::pow(2.0, -3.0 / 12.0), 8.0);

    // Tremolo: a 2 Hz square at full depth switches the note on and off.
    p = sinePatch();
    p[param::Lfo2Target] = static_cast<float>(dsp::LfoTarget::Amp);
    p[param::Lfo2Wave] = static_cast<float>(dsp::LfoWave::Square);
    p[param::Lfo2RateHz] = 2.0f;
    p[param::Lfo2Depth] = 1.0f;
    const auto tremolo = renderNote(p, 69, 0.6, 0.0, sr);
    CHECK_NEAR(peak(tremolo.left, at(0.05, sr), at(0.2, sr)), 1.0, 0.01);
    CHECK(peak(tremolo.left, at(0.3, sr), at(0.45, sr)) < 1.0e-6f);

    // Filter wobble: a 1 Hz square at depth 0.5 moves the cutoff +/- 2 octaves.
    p = sawPatch();
    p[param::FilterCutoffHz] = 1000.0f;
    p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Cutoff);
    p[param::Lfo1Wave] = static_cast<float>(dsp::LfoWave::Square);
    p[param::Lfo1RateHz] = 1.0f;
    p[param::Lfo1Depth] = 0.5f;
    const auto wobble = renderNote(p, 57, 1.0, 0.0, sr);
    const double open = harmonicIn(wobble, 0.2, 0.4, 2200.0);
    const double closed = harmonicIn(wobble, 0.7, 0.9, 2200.0);
    CHECK(open > 20.0 * closed);
    CHECK_NEAR(open, amplitudeAt(runOsc(Wave::Saw, 220.0 / sr, 9600), 2200.0 / sr) *
                         svfGain(FilterType::LowPass, sr, 4000.0, std::sqrt(2.0), 2200.0),
               0.002);

    // Pulse width: a square wave has no even harmonics until the LFO moves its width.
    p = sawPatch();
    p[param::Osc1Wave] = static_cast<float>(Wave::Square);
    const auto square = renderNote(p, 57, 0.5, 0.0, sr);
    CHECK(harmonicIn(square, 0.1, 0.4, 440.0) < 1.0e-3);
    p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::PulseWidth);
    p[param::Lfo1Wave] = static_cast<float>(dsp::LfoWave::Square);
    p[param::Lfo1RateHz] = 1.0f;
    p[param::Lfo1Depth] = 0.5f;
    const auto pulse = renderNote(p, 57, 0.5, 0.0, sr);
    CHECK(harmonicIn(pulse, 0.1, 0.4, 440.0) > 0.1);

    // A target of "off" or a depth of 0 changes nothing.
    p = sawPatch();
    const auto plain = renderNote(p, 57, 0.3, 0.1, sr);
    p[param::Lfo1Depth] = 1.0f;
    p[param::Lfo2Target] = static_cast<float>(dsp::LfoTarget::Pitch);
    p[param::Lfo2Depth] = 0.0f;
    CHECK(renderNote(p, 57, 0.3, 0.1, sr).left == plain.left);
}

void testPitchBend()
{
    const double sr = 48000.0;
    const auto bentHz = [&](float bend, float range) {
        Engine engine;
        engine.prepare(sr);
        auto p = sinePatch();
        p[param::BendRange] = range;
        engine.setParams(p);
        engine.setPitchBend(bend);
        engine.noteOn(69, 1.0f);
        std::vector<float> left(at(0.5, sr)), right(left.size());
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
        return estimateHz(left, sr, at(0.1, sr), at(0.4, sr));
    };

    CHECK_NEAR(bentHz(0.0f, 2.0f), 440.0, 0.05);
    CHECK_NEAR(bentHz(1.0f, 2.0f), 440.0 * std::pow(2.0, 2.0 / 12.0), 0.1);
    CHECK_NEAR(bentHz(-1.0f, 12.0f), 220.0, 0.05);
    CHECK_NEAR(bentHz(0.5f, 12.0f), 440.0 * std::pow(2.0, 6.0 / 12.0), 0.1);
}

// ---------------------------------------------------------------- effects

// Level of a held sine note at its own frequency, in dB relative to full scale.
double sineLevelDb(const ParamValues& p, int note, const MatchEqGains* matchEq = nullptr)
{
    const double sr = 48000.0;
    Preset preset;
    preset.params = p;
    if (matchEq)
        preset.matchEq = *matchEq;
    const NoteEvent events[] = {{0.0, 1.0, note, 1.0f}};
    RenderSettings settings;
    settings.sampleRate = sr;
    settings.tailSec = 0.0;
    const auto audio = render(preset, events, settings);
    // A whole number of cycles is not guaranteed here, so use a long window.
    const double a = amplitudeAt(slice(audio.left, 0.4, 0.9, sr), noteHz(note) / sr);
    return 20.0 * std::log10(a);
}

void testEqNode()
{
    const double sr = 48000.0;
    auto flat = sinePatch();
    auto p = flat;
    p[param::EqOn] = 1.0f;

    // Switched on with every gain at zero, the EQ changes nothing.
    const auto a = renderNote(flat, 57, 0.3, 0.0, sr);
    const auto b = renderNote(p, 57, 0.3, 0.0, sr);
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.left.size(); ++i)
        worst = std::max(worst, std::abs(a.left[i] - b.left[i]));
    CHECK(worst < 1.0e-6f);

    // Each band changes the level at a test note by what its formula predicts.
    p[param::EqLowFreqHz] = 300.0f;
    p[param::EqLowGainDb] = 12.0f;
    CHECK_NEAR(sineLevelDb(p, 45) - sineLevelDb(flat, 45),
               dsp::BiquadCoeffs::lowShelf(sr, 300.0, 12.0).gainDbAt(110.0, sr), 0.05);
    CHECK_NEAR(sineLevelDb(p, 45) - sineLevelDb(flat, 45), 12.0, 1.0); // well inside the shelf
    CHECK_NEAR(sineLevelDb(p, 105) - sineLevelDb(flat, 105), 0.0, 0.1); // far above it

    p = flat;
    p[param::EqOn] = 1.0f;
    p[param::EqMid1FreqHz] = 440.0f;
    p[param::EqMid1GainDb] = -9.0f;
    p[param::EqMid1Q] = 2.0f;
    CHECK_NEAR(sineLevelDb(p, 69) - sineLevelDb(flat, 69), -9.0, 0.05);

    p = flat;
    p[param::EqOn] = 1.0f;
    p[param::EqHighFreqHz] = 2000.0f;
    p[param::EqHighGainDb] = 6.0f;
    CHECK_NEAR(sineLevelDb(p, 105) - sineLevelDb(flat, 105),
               dsp::BiquadCoeffs::highShelf(sr, 2000.0, 6.0).gainDbAt(noteHz(105), sr), 0.05);
    CHECK_NEAR(sineLevelDb(p, 105) - sineLevelDb(flat, 105), 6.0, 0.6);
}

// A 10 ms blip, for hearing echoes and reverb tails.
ParamValues blipPatch()
{
    auto p = sinePatch();
    p[param::AmpAttackMs] = 0.1f;
    p[param::AmpDecayMs] = 10.0f;
    p[param::AmpSustain] = 0.0f;
    return p;
}

void testDelay()
{
    const double sr = 48000.0;
    const auto echo = [&](const std::vector<float>& x, double sec) { return peak(x, at(sec - 0.002, sr), at(sec + 0.02, sr)); };
    const auto run = [&](const ParamValues& p, double tempo = 120.0) {
        const NoteEvent events[] = {{0.0, 0.02, 81, 1.0f}};
        RenderSettings settings;
        settings.sampleRate = sr;
        settings.tailSec = 0.8;
        settings.tempoBpm = tempo;
        return render(p, events, settings);
    };

    auto p = blipPatch();
    const auto dry = run(p);
    const float blip = peak(dry.left, 0, at(0.02, sr));
    CHECK(blip > 0.5f);
    CHECK(peak(dry.left, at(0.05, sr), dry.numFrames()) == 0.0f);

    // Echoes every 100 ms, each half as loud as the one before.
    p[param::DelayOn] = 1.0f;
    p[param::DelayTimeMs] = 100.0f;
    p[param::DelayFeedback] = 0.5f;
    p[param::DelayMix] = 0.5f; // dry and wet both at full level
    const auto wet = run(p);
    CHECK_NEAR(peak(wet.left, 0, at(0.02, sr)), blip, 1e-6);
    CHECK_NEAR(echo(wet.left, 0.1), blip, 0.03 * blip);
    CHECK_NEAR(echo(wet.left, 0.2), 0.5 * blip, 0.03 * blip);
    CHECK_NEAR(echo(wet.left, 0.3), 0.25 * blip, 0.03 * blip);
    CHECK(peak(wet.left, at(0.03, sr), at(0.095, sr)) < 1.0e-3f); // silence between echoes
    CHECK(wet.left == wet.right);

    // A mix of 0 is the dry signal exactly.
    p[param::DelayMix] = 0.0f;
    CHECK(run(p).left == dry.left);

    // Ping-pong: left, right, left, ...
    p[param::DelayMix] = 0.5f;
    p[param::DelayMode] = 1.0f;
    const auto pong = run(p);
    CHECK_NEAR(echo(pong.left, 0.1), blip, 0.03 * blip);
    CHECK(echo(pong.right, 0.1) < 1.0e-3f);
    // Later echoes have been through the feedback filters twice or more, which
    // takes a few percent off such a short blip.
    CHECK_NEAR(echo(pong.right, 0.2), blip, 0.07 * blip);
    CHECK(echo(pong.left, 0.2) < 1.0e-3f);
    CHECK_NEAR(echo(pong.left, 0.3), 0.5 * blip, 0.07 * blip);

    // Spread makes the right channel's delay 50% longer.
    p[param::DelayMode] = 0.0f;
    p[param::DelaySpread] = 1.0f;
    const auto spread = run(p);
    CHECK_NEAR(echo(spread.left, 0.1), blip, 0.03 * blip);
    CHECK(echo(spread.right, 0.1) < 1.0e-3f);
    CHECK_NEAR(echo(spread.right, 0.15), blip, 0.03 * blip);

    // Tempo sync: an eighth note is 250 ms at 120 bpm and 500 ms at 60 bpm.
    p[param::DelaySpread] = 0.0f;
    p[param::DelaySync] = 3.0f;
    CHECK(choiceName(spec(param::DelaySync), 3.0f) == "1/8");
    CHECK_NEAR(echo(run(p, 120.0).left, 0.25), blip, 0.03 * blip);
    CHECK(echo(run(p, 120.0).left, 0.1) < 1.0e-3f);
    CHECK_NEAR(echo(run(p, 60.0).left, 0.5), blip, 0.03 * blip);

    // The high cut dulls each repeat: a 880 Hz blip through a 500 Hz cut gets quieter faster.
    p[param::DelaySync] = 0.0f;
    p[param::DelayHighCutHz] = 500.0f;
    const auto dull = run(p);
    CHECK(echo(dull.left, 0.2) < 0.5 * echo(wet.left, 0.2));

    // Modulation keeps the echoes but moves them, so the result differs.
    p[param::DelayHighCutHz] = 20000.0f;
    p[param::DelayModDepth] = 1.0f;
    p[param::DelayModRateHz] = 3.0f;
    const auto wobbly = run(p);
    CHECK(wobbly.left != wet.left);
    CHECK(wobbly.left != wobbly.right);
    CHECK(echo(wobbly.left, 0.1) > 0.5f * blip);
}

// The signal through a band-pass around hz (a two-pole resonator with a peak
// gain of 1, from the Audio EQ Cookbook, Q = 2).
std::vector<float> bandPass(const std::vector<float>& x, double sampleRate, double hz)
{
    const double w0 = 2.0 * kPi * hz / sampleRate, alpha = std::sin(w0) / 4.0, a0 = 1.0 + alpha;
    const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - alpha) / a0;
    std::vector<float> y(x.size());
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double out = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = out;
        y[i] = static_cast<float>(out);
    }
    return y;
}

// The largest bend in a stretch of signal (second difference): a click shows up as a spike.
double maxBend(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 2); i < std::min(to, x.size()); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
    return worst;
}

// The largest step from one sample to the next.
double maxStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - x[i - 1]));
    return worst;
}

void testReverb()
{
    const double sr = 48000.0;
    const auto run = [&](const ParamValues& p, double sampleRate = 48000.0) {
        const NoteEvent events[] = {{0.0, 0.03, 60, 1.0f}};
        RenderSettings settings;
        settings.sampleRate = sampleRate;
        settings.tailSec = 1.2;
        return render(p, events, settings);
    };

    // A short burst of noise into a fully wet reverb.
    auto p = blipPatch();
    p[param::Osc1Level] = 0.0f;
    p[param::NoiseLevel] = 1.0f;
    p[param::AmpDecayMs] = 20.0f;
    const auto dry = run(p);
    CHECK(peak(dry.left, at(0.1, sr), dry.numFrames()) == 0.0f);

    p[param::ReverbOn] = 1.0f;
    p[param::ReverbMix] = 1.0f;
    p[param::ReverbDamping] = 0.0f;
    p[param::ReverbPredelayMs] = 0.0f;

    // The tail falls 60 dB in the decay time: 36 dB over 0.6 s for a 1 s
    // decay. Measured on the whole signal, or around one frequency.
    const auto dropDb = [&](const StereoBuffer& a, double hz = 0.0) {
        const auto x = hz > 0.0 ? bandPass(a.left, a.sampleRate, hz) : a.left;
        const double early = rms(x, at(0.2, a.sampleRate), at(0.3, a.sampleRate));
        const double late = rms(x, at(0.8, a.sampleRate), at(0.9, a.sampleRate));
        return 20.0 * std::log10(early / late);
    };
    p[param::ReverbDecaySec] = 1.0f;
    const auto shortTail = run(p);
    CHECK_NEAR(dropDb(shortTail), 36.0, 4.0);
    p[param::ReverbDecaySec] = 3.0f;
    const auto longTail = run(p);
    CHECK_NEAR(dropDb(longTail), 12.0, 3.0);

    // Every mode keeps to the decay time in the mids.
    for (int mode = 0; mode < static_cast<int>(dsp::ReverbMode::Count); ++mode) {
        auto m = p;
        m[param::ReverbMode] = static_cast<float>(mode);
        m[param::ReverbDecaySec] = 1.0f;
        const double shortDrop = dropDb(run(m), 1000.0);
        m[param::ReverbDecaySec] = 3.0f;
        const double longDrop = dropDb(run(m), 1000.0);
        std::printf("reverb %s: 1 kHz drop over 0.6 s %.1f dB (1 s decay), %.1f dB (3 s)\n",
                    std::string(choiceName(spec(param::ReverbMode), static_cast<float>(mode))).c_str(), shortDrop, longDrop);
        CHECK_NEAR(shortDrop, 36.0, 4.0);
        CHECK_NEAR(longDrop, 12.0, 3.0);
    }

    // The room size doesn't change the decay time.
    p[param::ReverbSize] = 1.0f;
    CHECK_NEAR(dropDb(run(p)), 12.0, 3.0);
    p[param::ReverbSize] = 0.5f;

    // Damping makes the tail die sooner (treble first).
    p[param::ReverbDamping] = 0.9f;
    const auto damped = run(p);
    CHECK(rms(damped.left, at(0.8, sr), at(0.9, sr)) < 0.5 * rms(longTail.left, at(0.8, sr), at(0.9, sr)));

    // ... by the same amount at any size and sample rate: the treble decay is
    // set in seconds, not per trip round a line.
    p[param::ReverbDamping] = 0.4f;
    p[param::ReverbSize] = 0.0f;
    const double smallTreble = dropDb(run(p), 4000.0);
    p[param::ReverbSize] = 1.0f;
    const double bigTreble = dropDb(run(p), 4000.0);
    p[param::ReverbSize] = 0.5f;
    std::printf("reverb: 4 kHz drop at size 0 %.1f dB, at size 1 %.1f dB\n", smallTreble, bigTreble);
    CHECK(smallTreble > 16.0); // damped: more than the 12 dB of the mids
    CHECK_NEAR(smallTreble, bigTreble, 2.0);
    const double treble48 = dropDb(run(p), 2000.0);
    const double treble24 = dropDb(run(p, 24000.0), 2000.0);
    std::printf("reverb: 2 kHz drop at 48 kHz %.1f dB, at 24 kHz %.1f dB\n", treble48, treble24);
    CHECK_NEAR(treble48, treble24, 2.0);
    p[param::ReverbDamping] = 0.0f;

    // Bass decay: x2 makes the lows ring on.
    p[param::ReverbDecaySec] = 1.0f;
    const double bass = dropDb(run(p), 100.0);
    p[param::ReverbBassMult] = 2.0f;
    const double longBass = dropDb(run(p), 100.0);
    std::printf("reverb: 100 Hz drop %.1f dB, with bass decay x2 %.1f dB\n", bass, longBass);
    CHECK(longBass < 0.7 * bass);
    p[param::ReverbBassMult] = 1.0f;
    p[param::ReverbDecaySec] = 3.0f;

    // The high cut darkens what goes in.
    const auto bright = rms(bandPass(longTail.left, sr, 8000.0), at(0.1, sr), at(0.5, sr));
    p[param::ReverbHighCutHz] = 3000.0f;
    const auto cut = rms(bandPass(run(p).left, sr, 8000.0), at(0.1, sr), at(0.5, sr));
    CHECK(cut < 0.4 * bright);
    p[param::ReverbHighCutHz] = 20000.0f;

    // Stereo by default, mono at a width of 0. Width doesn't change the mono sum.
    CHECK(longTail.left != longTail.right);
    p[param::ReverbWidth] = 0.0f;
    const auto narrow = run(p);
    CHECK(narrow.left == narrow.right);
    double sumDiff = 0.0;
    for (std::size_t i = 0; i < narrow.numFrames(); ++i)
        sumDiff = std::max(sumDiff, std::abs(static_cast<double>(narrow.left[i]) + narrow.right[i] - longTail.left[i] -
                                             longTail.right[i]));
    CHECK(sumDiff < 1.0e-5);
    p[param::ReverbWidth] = 1.0f;

    // The wobble is seeded: the same tail every time, but not the tail without it.
    p[param::ReverbModDepth] = 1.0f;
    const auto wobbly = run(p);
    CHECK(run(p).left == wobbly.left);
    CHECK(wobbly.left != longTail.left);
    p[param::ReverbModDepth] = spec(param::ReverbModDepth).defaultValue;

    // Pre-delay holds the reverb back.
    p[param::ReverbPredelayMs] = 100.0f;
    const auto late = run(p);
    CHECK(peak(late.left, 0, at(0.095, sr)) == 0.0f);
    CHECK(peak(late.left, at(0.1, sr), at(0.3, sr)) > 1.0e-3f);
    p[param::ReverbPredelayMs] = 0.0f;

    // A mix of 0 is the dry signal exactly.
    p[param::ReverbMix] = 0.0f;
    CHECK(run(p).left == dry.left);

    // How loud is the reverb next to the dry sound? Held saw, fully wet.
    auto level = sawPatch();
    const auto drySaw = renderNote(level, 57, 2.0, 0.0, sr);
    level[param::ReverbOn] = 1.0f;
    level[param::ReverbMix] = 1.0f;
    const auto wetSaw = renderNote(level, 57, 2.0, 0.0, sr);
    const double wetDb = 20.0 * std::log10(rms(wetSaw.left, at(1.5, sr), at(2.0, sr)) / rms(drySaw.left, at(1.5, sr), at(2.0, sr)));
    std::printf("reverb: fully wet level for a held note at the default settings: %.1f dB relative to dry\n", wetDb);
    CHECK_NEAR(wetDb, -3.0, 4.0);

    // Every mode is about as loud as the others, on a held chord of saws and
    // on filtered noise. (One note alone can land on a peak or a dip of a mode.)
    const auto modeDb = [&](int mode) {
        double sum = 0.0;
        for (const bool noise : {false, true}) {
            auto q = level;
            q[param::ReverbMode] = static_cast<float>(mode);
            if (noise) {
                q[param::Osc1Level] = 0.0f;
                q[param::NoiseLevel] = 0.5f;
                q[param::FilterCutoffHz] = 1500.0f;
            }
            const NoteEvent chord[] = {{0.0, 2.0, 45, 1.0f}, {0.0, 2.0, 52, 1.0f}, {0.0, 2.0, 57, 1.0f}, {0.0, 2.0, 64, 1.0f}};
            RenderSettings settings;
            settings.tailSec = 0.0;
            const auto wet = render(q, chord, settings);
            const double l = rms(wet.left, at(1.2, sr), at(2.0, sr)), r = rms(wet.right, at(1.2, sr), at(2.0, sr));
            sum += 10.0 * std::log10(l * l + r * r);
        }
        return sum / 2.0;
    };
    const double hallDb = modeDb(static_cast<int>(dsp::ReverbMode::Hall));
    for (int mode = 0; mode < static_cast<int>(dsp::ReverbMode::Count); ++mode) {
        const double db = modeDb(mode) - hallDb;
        std::printf("reverb %s: %+.1f dB against hall\n",
                    std::string(choiceName(spec(param::ReverbMode), static_cast<float>(mode))).c_str(), db);
        CHECK_NEAR(db, 0.0, 2.0);
    }
}

// Plays a patch through an engine in 256-sample blocks, calling
// change(engine, seconds) before each block. One note, from 0 to noteSec.
StereoBuffer play(const ParamValues& p, int note, double noteSec, double totalSec,
                  const std::function<void(Engine&, double)>& change = {})
{
    const double sr = 48000.0;
    Engine engine;
    engine.prepare(sr);
    engine.setParams(p);
    StereoBuffer out;
    out.sampleRate = sr;
    const std::size_t total = at(totalSec, sr), block = 256;
    out.left.resize(total);
    out.right.resize(total);
    engine.noteOn(note, 1.0f);
    bool held = true;
    for (std::size_t start = 0; start < total; start += block) {
        const double now = static_cast<double>(start) / sr;
        if (held && now >= noteSec) {
            engine.noteOff(note);
            held = false;
        }
        if (change)
            change(engine, now);
        const auto n = static_cast<int>(std::min(block, total - start));
        engine.process(out.left.data() + start, out.right.data() + start, n);
    }
    return out;
}

void testReverbGlides()
{
    const double sr = 48000.0;
    // A sine through a fully wet reverb: a tonal tail, where clicks are easy to hear.
    auto p = sinePatch();
    p[param::AmpReleaseMs] = 5.0f;
    p[param::ReverbOn] = 1.0f;
    p[param::ReverbMix] = 1.0f;
    p[param::ReverbDecaySec] = 3.0f;
    p[param::ReverbDamping] = 0.6f;
    const auto steady = play(p, 57, 0.4, 1.2);
    const double tailFrom = 0.5, tailTo = 1.2;
    const double calm = maxBend(steady.left, at(tailFrom, sr), at(tailTo, sr));
    CHECK(calm > 0.0);

    // Dragging the size while the tail rings, a step every block, bends the
    // pitch a little but doesn't click.
    const auto drag = play(p, 57, 0.4, 1.2, [](Engine& e, double now) {
        if (now >= 0.5 && now < 0.8)
            e.setParam(param::ReverbSize, 0.5f + 0.27f * static_cast<float>(now - 0.5));
    });
    const double dragged = maxBend(drag.left, at(tailFrom, sr), at(tailTo, sr));
    // One big jump, as when a preset is loaded.
    const auto jump = play(p, 57, 0.4, 1.2, [](Engine& e, double now) {
        if (now >= 0.6)
            e.setParam(param::ReverbSize, 0.9f);
    });
    const double jumped = maxBend(jump.left, at(tailFrom, sr), at(tailTo, sr));
    std::printf("reverb: tail bend %.5f, dragging the size %.5f, a size jump %.5f\n", calm, dragged, jumped);
    CHECK(dragged < 3.0 * calm);
    CHECK(jumped < 3.0 * calm);

    // Pre-delay changes while the note plays glide too.
    const double heldCalm = maxBend(steady.left, at(0.1, sr), at(0.4, sr));
    const auto predelay = play(p, 57, 0.4, 1.2, [](Engine& e, double now) {
        if (now >= 0.1 && now < 0.4)
            e.setParam(param::ReverbPredelayMs, 10.0f + 100.0f * static_cast<float>(now - 0.1));
    });
    CHECK(maxBend(predelay.left, at(0.1, sr), at(0.4, sr)) < 3.0 * heldCalm);

    // A new mode fades the old tail out over a few ms instead of cutting it.
    const auto mode = play(p, 57, 0.4, 1.2, [](Engine& e, double now) {
        if (now >= 0.6)
            e.setParam(param::ReverbMode, static_cast<float>(dsp::ReverbMode::Plate));
    });
    const double calmStep = maxStep(steady.left, at(tailFrom, sr), at(tailTo, sr));
    CHECK(maxStep(mode.left, at(tailFrom, sr), at(tailTo, sr)) < 1.5 * calmStep);
    // ... and the new mode is empty: with no input, it stays silent.
    CHECK(peak(mode.left, at(0.7, sr), at(1.2, sr)) == 0.0f);
}

void testEffectSwitching()
{
    const double sr = 48000.0;
    auto p = sinePatch();
    p[param::AmpReleaseMs] = 5.0f;
    p[param::ReverbOn] = 1.0f;
    p[param::ReverbMix] = 0.5f;
    p[param::ReverbDecaySec] = 2.0f;
    p[param::DelayTimeMs] = 120.0f;
    p[param::DelayFeedback] = 0.6f;
    p[param::DelayMix] = 0.5f;
    const auto steady = play(p, 57, 0.3, 1.0);
    const double calmStep = maxStep(steady.left, at(0.35, sr), at(1.0, sr));

    // Switching the reverb off while its tail rings fades it out (no click),
    // then leaves the output silent.
    const auto off = play(p, 57, 0.3, 1.0, [](Engine& e, double now) {
        if (now >= 0.5)
            e.setParam(param::ReverbOn, 0.0f);
    });
    CHECK(maxStep(off.left, at(0.35, sr), at(1.0, sr)) < 1.5 * calmStep);
    CHECK(peak(off.left, at(0.52, sr), at(1.0, sr)) == 0.0f);
    CHECK(peak(off.left, at(0.49, sr), at(0.5, sr)) > 0.01f);

    // Switched back on, it starts empty: the old tail doesn't come back.
    const auto again = play(p, 57, 0.3, 1.0, [](Engine& e, double now) {
        e.setParam(param::ReverbOn, now >= 0.5 && now < 0.6 ? 0.0f : 1.0f);
    });
    CHECK(peak(again.left, at(0.6, sr), at(1.0, sr)) == 0.0f);

    // Switching it on mid-note fades it in.
    auto later = p;
    later[param::ReverbOn] = 0.0f;
    const auto on = play(later, 57, 0.6, 1.0, [](Engine& e, double now) {
        if (now >= 0.3)
            e.setParam(param::ReverbOn, 1.0f);
    });
    CHECK(maxStep(on.left, at(0.29, sr), at(0.33, sr)) < 1.5 * maxStep(on.left, at(0.2, sr), at(0.29, sr)));
    CHECK(peak(on.left, at(0.5, sr), at(0.6, sr)) > 1.2f * peak(on.left, at(0.2, sr), at(0.29, sr))); // the reverb is in

    // The delay: off in the middle of an echo fades out; back on, no stale
    // echoes from before (its lines aren't cleared, only marked as old).
    p[param::ReverbOn] = 0.0f;
    p[param::DelayOn] = 1.0f;
    const auto echoes = play(p, 57, 0.05, 1.0);
    const auto delayOff = play(p, 57, 0.05, 1.0, [](Engine& e, double now) {
        e.setParam(param::DelayOn, now >= 0.25 && now < 0.35 ? 0.0f : 1.0f);
    });
    CHECK(peak(echoes.left, at(0.36, sr), at(1.0, sr)) > 0.05f);
    CHECK(peak(echoes.left, at(0.25, sr), at(0.26, sr)) > 0.05f); // mid-echo
    CHECK(maxStep(delayOff.left, at(0.2, sr), at(0.35, sr)) < 1.5 * maxStep(echoes.left, at(0.2, sr), at(0.35, sr)));
    CHECK(peak(delayOff.left, at(0.27, sr), at(1.0, sr)) == 0.0f);
}

void testMatchEq()
{
    const double sr = 48000.0;
    const auto flat = sinePatch();

    // All gains zero: no change at all.
    MatchEqGains gains{};
    CHECK_NEAR(sineLevelDb(flat, 69, &gains), sineLevelDb(flat, 69), 1e-9);

    // An uneven curve: the level at each test note follows the bank's response.
    for (std::size_t band = 0; band < gains.size(); ++band)
        gains[band] = 6.0f * static_cast<float>(std::sin(0.7 * static_cast<double>(band)));
    const auto coeffs = dsp::matchEqCoeffs(sr, gains);
    for (const int note : {33, 57, 69, 81, 100}) {
        const double expected = dsp::matchEqGainDbAt(coeffs, noteHz(note), sr);
        CHECK_NEAR(sineLevelDb(flat, note, &gains) - sineLevelDb(flat, note), expected, 0.05);
    }

    // The amount parameter scales the band gains.
    auto half = flat;
    half[param::MatchEqAmount] = 0.5f;
    const auto halfCoeffs = dsp::matchEqCoeffs(sr, gains, 0.5);
    CHECK_NEAR(sineLevelDb(half, 69, &gains) - sineLevelDb(flat, 69), dsp::matchEqGainDbAt(halfCoeffs, 440.0, sr), 0.05);
    auto none = flat;
    none[param::MatchEqAmount] = 0.0f;
    CHECK_NEAR(sineLevelDb(none, 69, &gains), sineLevelDb(flat, 69), 1e-9);

    // Band centres: three per octave, band 17 at 1 kHz.
    CHECK_NEAR(dsp::matchEqBandHz(17), 1000.0, 1e-9);
    CHECK_NEAR(dsp::matchEqBandHz(20), 2000.0, 1e-9);
    CHECK_NEAR(dsp::matchEqBandHz(0), 19.686, 0.001);
}

void testEverythingAtOnce()
{
    // All features on at extreme settings must stay finite at every sample rate.
    for (const double sr : {22050.0, 44100.0, 48000.0, 96000.0}) {
        auto p = defaultValues();
        p[param::GainDb] = 0.0f;
        p[param::Osc2Level] = 1.0f;
        p[param::Osc3Level] = 1.0f;
        p[param::Osc3Wave] = static_cast<float>(Wave::Square);
        p[param::NoiseLevel] = 0.5f;
        p[param::UnisonVoices] = 7.0f;
        p[param::UnisonDetune] = 50.0f;
        p[param::UnisonSpread] = 1.0f;
        p[param::FilterResonance] = 1.0f;
        p[param::FilterCutoffHz] = 2000.0f;
        p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Pitch);
        p[param::Lfo1Depth] = 1.0f;
        p[param::Lfo1RateHz] = 30.0f;
        p[param::Lfo2Target] = static_cast<float>(dsp::LfoTarget::Cutoff);
        p[param::Lfo2Wave] = static_cast<float>(dsp::LfoWave::Random);
        p[param::Lfo2Depth] = 1.0f;
        p[param::Lfo2RateHz] = 30.0f;
        p[param::EqOn] = 1.0f;
        p[param::EqLowGainDb] = 18.0f;
        p[param::EqHighGainDb] = 18.0f;
        p[param::EqHighFreqHz] = 18000.0f;
        p[param::DelayOn] = 1.0f;
        p[param::DelayTimeMs] = 2000.0f;
        p[param::DelaySpread] = 1.0f;
        p[param::DelayFeedback] = 0.95f;
        p[param::DelayModDepth] = 1.0f;
        p[param::DelayModRateHz] = 10.0f;
        p[param::ReverbOn] = 1.0f;
        p[param::ReverbSize] = 1.0f;
        p[param::ReverbDecaySec] = 20.0f;
        p[param::ReverbMix] = 1.0f;
        p[param::ReverbModDepth] = 1.0f;
        p[param::ReverbModRateHz] = 5.0f;
        p[param::ReverbDiffusion] = 1.0f;
        p[param::ReverbBassMult] = 2.0f;
        p[param::ReverbPredelayMs] = 200.0f;

        // Every reverb mode.
        for (int mode = 0; mode < static_cast<int>(dsp::ReverbMode::Count); ++mode) {
            p[param::ReverbMode] = static_cast<float>(mode);
            Preset preset;
            preset.params = p;
            preset.matchEq.fill(18.0f);

            const NoteEvent events[] = {{0.0, 0.3, 24, 1.0f}, {0.05, 0.3, 127, 1.0f}, {0.1, 0.2, 60, 1.0f}};
            RenderSettings settings;
            settings.sampleRate = sr;
            settings.tailSec = 0.3;
            const auto audio = render(preset, events, settings);
            bool finite = true;
            for (std::size_t i = 0; i < audio.numFrames(); ++i)
                finite = finite && std::isfinite(audio.left[i]) && std::isfinite(audio.right[i]);
            CHECK(finite);
        }
    }
}

// ---------------------------------------------------------------- presets

void testJson()
{
    std::string error;
    const auto doc = Json::parse(R"({"a": 1.5, "b": [1, 2, {"c": "x\ny"}], "t": true, "n": null, "u": "é",})", &error);
    CHECK(doc.has_value());
    if (doc) {
        CHECK(doc->find("a") && doc->find("a")->number() == 1.5);
        CHECK(doc->find("b") && doc->find("b")->items().size() == 3);
        CHECK(doc->find("b")->items()[2].find("c")->string() == "x\ny");
        CHECK(doc->find("t")->boolean());
        CHECK(doc->find("n")->isNull());
        CHECK(doc->find("u")->string() == "\xC3\xA9");
        CHECK(!doc->find("missing"));

        // Writing and reading again gives the same document.
        const auto again = Json::parse(doc->dump(2));
        CHECK(again && again->dump(0) == doc->dump(0));
    }

    for (const char* bad : {"", "{", "{\"a\" 1}", "[1, 2", "{\"a\": tru}", "\"open", "{} extra", "nope"}) {
        error.clear();
        CHECK(!Json::parse(bad, &error));
        CHECK(!error.empty());
    }

    CHECK(formatNumber(0.3f) == "0.3");
    CHECK(formatNumber(350.0) == "350");
    CHECK(formatNumber(-12.5) == "-12.5");
    CHECK(formatNumber(20000.0) == "20000");
}

void testPreset()
{
    Preset preset;
    preset.name = "Test \"pad\"";
    preset.params[param::Osc1Wave] = static_cast<float>(Wave::Square);
    preset.params[param::FilterCutoffHz] = 1234.5f;
    preset.params[param::FilterType] = static_cast<float>(FilterType::BandPass);
    preset.params[param::ReverbOn] = 1.0f;
    preset.params[param::DelaySync] = 4.0f;
    preset.params[param::UnisonVoices] = 5.0f;
    preset.params[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Cutoff);
    preset.params[param::AmpAttackMs] = 0.1f;
    for (std::size_t band = 0; band < preset.matchEq.size(); ++band)
        preset.matchEq[band] = static_cast<float>(band) * 0.25f - 3.0f;

    // Write, read back, compare.
    const std::string text = presetToJson(preset);
    std::string error;
    std::vector<std::string> warnings;
    const auto loaded = presetFromJson(text, &error, &warnings);
    CHECK(loaded.has_value());
    CHECK(warnings.empty());
    if (loaded) {
        CHECK(loaded->name == preset.name);
        for (int i = 0; i < kNumParams; ++i) {
            const auto k = static_cast<std::size_t>(i);
            CHECK_NEAR(loaded->params[k], preset.params[k], 1e-5 * std::max(1.0f, std::abs(preset.params[k])));
        }
        for (std::size_t band = 0; band < preset.matchEq.size(); ++band)
            CHECK_NEAR(loaded->matchEq[band], preset.matchEq[band], 0.006);
    }

    // The file is grouped by node and uses names for choices.
    const auto root = Json::parse(text);
    CHECK(root.has_value());
    if (root) {
        const Json* nodes = root->find("nodes");
        CHECK(nodes && nodes->find("osc1") && nodes->find("osc1")->find("wave")->string() == "square");
        CHECK(nodes->find("filter")->find("type")->string() == "bandpass");
        CHECK(nodes->find("filter")->find("cutoff_hz")->number() == 1234.5);
        CHECK(nodes->find("reverb")->find("on")->string() == "on");
        CHECK(nodes->find("delay")->find("sync")->string() == "1/8D");
        CHECK(nodes->find("master")->find("gain_db")->number() == -12.0);
        CHECK(nodes->find("matcheq")->find("gains_db")->items().size() == preset.matchEq.size());
        CHECK(nodes->members().size() == nodeNames().size());
    }

    // A partial file changes only what it mentions. Values can be names,
    // numbers, numbers in strings or true/false; "params" is the flat form.
    warnings.clear();
    const auto partial = presetFromJson(R"({
        "nodes": { "filter": { "cutoff_hz": 900, "type": "highpass", "resonance": "0.5" },
                   "delay": { "on": true, "sync": "1/4" },
                   "nope": { "x": 1 },
                   "osc1": { "banana": 2, "level": 7 } },
        "params": { "reverb_mix": 0.4, "not_a_param": 1 }
    })", &error, &warnings, preset);
    CHECK(partial.has_value());
    if (partial) {
        CHECK(partial->params[param::FilterCutoffHz] == 900.0f);
        CHECK(partial->params[param::FilterType] == static_cast<float>(FilterType::HighPass));
        CHECK(partial->params[param::FilterResonance] == 0.5f);
        CHECK(partial->params[param::DelayOn] == 1.0f);
        CHECK(partial->params[param::DelaySync] == 5.0f);
        CHECK(partial->params[param::ReverbMix] == 0.4f);
        CHECK(partial->params[param::Osc1Level] == 1.0f); // 7 was limited to the maximum
        CHECK(partial->params[param::UnisonVoices] == 5.0f); // untouched
        CHECK(partial->name == preset.name);
        CHECK(warnings.size() == 4); // nope.x, osc1.banana, osc1.level limited, not_a_param
    }

    CHECK(!presetFromJson("not json", &error));
    CHECK(!presetFromJson("[1, 2]", &error));

    // Save and load a file.
    const auto path = std::filesystem::temp_directory_path() / "synthmatch_test_preset.json";
    CHECK(savePreset(path, preset, &error));
    const auto fromFile = loadPreset(path, &error);
    CHECK(fromFile && fromFile->params == loaded->params);
    std::filesystem::remove(path);
    CHECK(!loadPreset(path, &error));
}

// ---------------------------------------------------------------- golden renders

struct GoldenCase {
    const char* name;
    ParamValues params;
    std::vector<NoteEvent> notes;
    bool stereo = false; // the file holds the left channel, then the right
};

std::vector<GoldenCase> goldenCases()
{
    std::vector<GoldenCase> cases;
    // All but the effects chain are dry: they were recorded before the
    // default patch had its touch of reverb.

    // The default patch: one saw.
    cases.push_back({"default_saw_c3", dryDefaults(), {{0.0, 0.25, 48, 0.8f}}});

    {
        auto p = dryDefaults();
        p[param::Osc1Wave] = static_cast<float>(Wave::Square);
        p[param::Osc1PulseWidth] = 0.3f;
        p[param::AmpAttackMs] = 20.0f;
        p[param::AmpDecayMs] = 80.0f;
        p[param::AmpSustain] = 0.6f;
        cases.push_back({"pulse_a2", p, {{0.0, 0.25, 45, 1.0f}}});
    }

    {
        // Three detuned oscillators playing a chord with overlapping notes.
        auto p = dryDefaults();
        p[param::Osc2Level] = 0.6f;
        p[param::Osc2Wave] = static_cast<float>(Wave::Square);
        p[param::Osc2Coarse] = 12.0f;
        p[param::Osc2Fine] = -8.0f;
        p[param::Osc3Level] = 0.8f;
        p[param::Osc3Wave] = static_cast<float>(Wave::Triangle);
        p[param::Osc3Coarse] = -12.0f;
        p[param::Osc3Fine] = 6.0f;
        p[param::AmpReleaseMs] = 60.0f;
        cases.push_back(
            {"three_osc_chord", p, {{0.0, 0.2, 60, 0.9f}, {0.05, 0.2, 64, 0.7f}, {0.1, 0.15, 67, 0.8f}}});
    }

    {
        // Noise plus a sine, decaying to silence with the key held.
        auto p = dryDefaults();
        p[param::Osc1Wave] = static_cast<float>(Wave::Sine);
        p[param::Osc1Level] = 0.5f;
        p[param::NoiseLevel] = 0.8f;
        p[param::AmpAttackMs] = 0.5f;
        p[param::AmpDecayMs] = 120.0f;
        p[param::AmpSustain] = 0.0f;
        cases.push_back({"noise_pluck", p, {{0.0, 0.25, 72, 1.0f}}});
    }

    {
        // A resonant low-pass swept down by the filter envelope.
        auto p = dryDefaults();
        p[param::FilterCutoffHz] = 200.0f;
        p[param::FilterResonance] = 0.65f;
        p[param::FilterEnvAmount] = 5.0f;
        p[param::FilterAttackMs] = 2.0f;
        p[param::FilterDecayMs] = 150.0f;
        p[param::FilterSustain] = 0.2f;
        p[param::FilterReleaseMs] = 80.0f;
        cases.push_back({"lowpass_sweep_e2", p, {{0.0, 0.25, 40, 0.9f}}});
    }

    {
        // A key-tracked high-pass on a square and noise, with overlapping notes.
        auto p = dryDefaults();
        p[param::Osc1Wave] = static_cast<float>(Wave::Square);
        p[param::NoiseLevel] = 0.3f;
        p[param::FilterType] = static_cast<float>(FilterType::HighPass);
        p[param::FilterCutoffHz] = 700.0f;
        p[param::FilterResonance] = 0.4f;
        p[param::FilterKeyTrack] = 1.0f;
        p[param::AmpReleaseMs] = 60.0f;
        cases.push_back({"highpass_keytrack", p, {{0.0, 0.2, 52, 0.8f}, {0.08, 0.17, 64, 0.9f}}});
    }

    {
        // A narrow band-pass that the envelope moves downwards.
        auto p = dryDefaults();
        p[param::Osc2Level] = 0.7f;
        p[param::Osc2Coarse] = 7.0f;
        p[param::FilterType] = static_cast<float>(FilterType::BandPass);
        p[param::FilterCutoffHz] = 2500.0f;
        p[param::FilterResonance] = 0.8f;
        p[param::FilterEnvAmount] = -2.5f;
        p[param::FilterAttackMs] = 60.0f;
        p[param::FilterSustain] = 0.8f;
        cases.push_back({"bandpass_res_g3", p, {{0.0, 0.25, 55, 1.0f}}});
    }

    {
        // A wide detuned stack with vibrato and a random filter wobble.
        auto p = dryDefaults();
        p[param::UnisonVoices] = 5.0f;
        p[param::UnisonDetune] = 22.0f;
        p[param::UnisonSpread] = 0.8f;
        p[param::Osc2Level] = 0.5f;
        p[param::Osc2Wave] = static_cast<float>(Wave::Square);
        p[param::Osc2Coarse] = -12.0f;
        p[param::FilterCutoffHz] = 1800.0f;
        p[param::FilterResonance] = 0.3f;
        p[param::Lfo1Target] = static_cast<float>(dsp::LfoTarget::Pitch);
        p[param::Lfo1RateHz] = 6.0f;
        p[param::Lfo1Depth] = 0.15f;
        p[param::Lfo2Target] = static_cast<float>(dsp::LfoTarget::Cutoff);
        p[param::Lfo2Wave] = static_cast<float>(dsp::LfoWave::Random);
        p[param::Lfo2RateHz] = 12.0f;
        p[param::Lfo2Depth] = 0.3f;
        p[param::AmpAttackMs] = 30.0f;
        cases.push_back({"unison_lfo_pad", p, {{0.0, 0.25, 57, 0.9f}, {0.04, 0.2, 64, 0.8f}}, true});
    }

    {
        // The effects chain: EQ, ping-pong delay with modulation, and reverb.
        auto p = defaultValues();
        p[param::AmpDecayMs] = 60.0f;
        p[param::AmpSustain] = 0.0f;
        p[param::FilterCutoffHz] = 3000.0f;
        p[param::EqOn] = 1.0f;
        p[param::EqLowGainDb] = 4.0f;
        p[param::EqMid2GainDb] = -5.0f;
        p[param::EqHighGainDb] = 3.0f;
        p[param::DelayOn] = 1.0f;
        p[param::DelayTimeMs] = 90.0f;
        p[param::DelayFeedback] = 0.5f;
        p[param::DelayMix] = 0.4f;
        p[param::DelayMode] = 1.0f;
        p[param::DelayHighCutHz] = 5000.0f;
        p[param::DelayModDepth] = 0.3f;
        p[param::ReverbOn] = 1.0f;
        p[param::ReverbMix] = 0.35f;
        p[param::ReverbDecaySec] = 1.2f;
        cases.push_back({"fx_chain_pluck", p, {{0.0, 0.1, 62, 1.0f}, {0.12, 0.1, 69, 0.8f}}, true});
    }

    return cases;
}

void testGoldenRenders()
{
    const std::filesystem::path dir = SYNTHMATCH_GOLDEN_DIR;
    constexpr float tolerance = 1.0e-4f; // allows for small math-library differences between platforms

    for (const auto& c : goldenCases()) {
        RenderSettings settings;
        settings.tailSec = 0.15;
        const auto rendered = render(c.params, c.notes, settings);
        struct {
            std::vector<float> left;
        } audio{rendered.left};
        if (c.stereo)
            audio.left.insert(audio.left.end(), rendered.right.begin(), rendered.right.end());
        const auto path = dir / (std::string(c.name) + ".f32");
        const auto bytes = static_cast<std::streamsize>(audio.left.size() * sizeof(float));

        if (g_updateGolden) {
            std::filesystem::create_directories(dir);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(audio.left.data()), bytes);
            CHECK(out.good());
            std::printf("updated %s\n", path.string().c_str());
            continue;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "golden file missing: %s (run with --update-golden)\n", path.string().c_str());
            ++g_failures;
            continue;
        }

        std::vector<float> expected(audio.left.size());
        in.read(reinterpret_cast<char*>(expected.data()), bytes);
        const bool sameLength = in.gcount() == bytes && in.peek() == std::ifstream::traits_type::eof();
        if (!sameLength) {
            std::fprintf(stderr, "golden %s: length differs from the render\n", c.name);
            ++g_failures;
            continue;
        }

        float maxDiff = 0.0f;
        for (std::size_t i = 0; i < expected.size(); ++i)
            maxDiff = std::max(maxDiff, std::abs(expected[i] - audio.left[i]));
        if (!(maxDiff <= tolerance)) {
            std::fprintf(stderr, "golden %s: max difference %g exceeds %g\n", c.name, static_cast<double>(maxDiff),
                         static_cast<double>(tolerance));
            ++g_failures;
        }
    }
}

// ---------------------------------------------------------------- files

void testWavFile()
{
    const auto audio = renderNote(defaultValues(), 60, 0.25, 0.25);
    const auto path = std::filesystem::temp_directory_path() / "synthmatch_test.wav";

    std::string error;
    CHECK(writeWav(path, audio, &error));
    // 12-byte RIFF header + 26-byte fmt + 12-byte fact + 8-byte data header + samples.
    CHECK(std::filesystem::file_size(path) == 58 + audio.numFrames() * 8);

    // Reading it back gives the same samples.
    const auto back = readWav(path, &error);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->sampleRate == audio.sampleRate);
        CHECK(back->left == audio.left);
        CHECK(back->right == audio.right);
    }
    std::filesystem::remove(path);
    CHECK(!readWav(path, &error));

    // A hand-made 16-bit mono PCM file: 4 samples at 8 kHz.
    const unsigned char pcm[] = {'R', 'I', 'F', 'F', 44,  0,   0,   0,   'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16,  0,
                                 0,   0,   1,   0,   1,   0,   0x40, 0x1F, 0,  0,   0x80, 0x3E, 0,  0,   2,   0,   16,  0,
                                 'd', 'a', 't', 'a', 8,   0,   0,   0,   0,   0,   0,   0x40, 0,  0x80, 0xFF, 0x7F};
    const auto pcmPath = std::filesystem::temp_directory_path() / "synthmatch_test_pcm.wav";
    {
        std::ofstream out(pcmPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(pcm), sizeof pcm);
    }
    const auto mono = readWav(pcmPath, &error);
    CHECK(mono.has_value());
    if (mono) {
        CHECK(mono->sampleRate == 8000.0);
        CHECK(mono->numFrames() == 4);
        CHECK(mono->left == std::vector<float>({0.0f, 0.5f, -1.0f, 32767.0f / 32768.0f}));
        CHECK(mono->right == mono->left);
    }
    std::filesystem::remove(pcmPath);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--update-golden") == 0)
            g_updateGolden = true;

    testSchema();
    testOscillatorHarmonics();
    testOscillatorAliasing();
    testOversampling();
    testAdsrShape();
    testSilenceWithoutNotes();
    testDeterminism();
    testPitch();
    testTuning();
    testGainAndVelocity();
    testMixer();
    testNoise();
    testEnvelopeInEngine();
    testSustainZeroFreesVoice();
    testVoiceStealing();
    testExtremeNotes();
    testFilterResponse();
    testFilterInEngine();
    testFilterKeyTracking();
    testFilterEnvelope();
    testBlockSizeIndependence();
    testFilterStability();
    testUnison();
    testLfo();
    testPitchBend();
    testEqNode();
    testDelay();
    testReverb();
    testReverbGlides();
    testEffectSwitching();
    testMatchEq();
    testEverythingAtOnce();
    testJson();
    testPreset();
    testGoldenRenders();
    testWavFile();

    if (g_failures == 0)
        std::puts(g_updateGolden ? "Golden files updated; all other tests passed." : "All engine tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
