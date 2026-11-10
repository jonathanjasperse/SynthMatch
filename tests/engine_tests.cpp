// Engine tests. Plain asserts, no framework; run through CTest.
//
// Golden renders live in tests/golden. After an intentional change to the
// sound, regenerate them with:  synthmatch_engine_tests --update-golden

#include "synthmatch/Engine.h"
#include "synthmatch/Params.h"
#include "synthmatch/Render.h"
#include "synthmatch/dsp/Envelope.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Oscillator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// A patch with only oscillator 1, as a clean starting point for tests.
ParamValues sinePatch()
{
    auto p = defaultValues();
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
    auto p = defaultValues();
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
    const auto raw = runOsc(Wave::Saw, hz / sr, steady.size());

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
    CHECK(kNumParams == 30);
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

    const NoteEvent notes[] = {{0.0, 0.2, 48, 0.9f}, {0.0501, 0.2, 55, 0.7f}, {0.13, 0.1, 64, 0.8f}};
    std::vector<float> reference;
    for (const int blockSize : {256, 1, 32, 1000}) {
        RenderSettings settings;
        settings.blockSize = blockSize;
        settings.tailSec = 0.2;
        const auto audio = render(p, notes, settings);
        if (reference.empty()) {
            reference = audio.left;
            CHECK(peak(reference, 0, reference.size()) > 0.05f);
        }
        CHECK(audio.left == reference);
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

// ---------------------------------------------------------------- golden renders

struct GoldenCase {
    const char* name;
    ParamValues params;
    std::vector<NoteEvent> notes;
};

std::vector<GoldenCase> goldenCases()
{
    std::vector<GoldenCase> cases;

    // The default patch: one saw.
    cases.push_back({"default_saw_c3", defaultValues(), {{0.0, 0.25, 48, 0.8f}}});

    {
        auto p = defaultValues();
        p[param::Osc1Wave] = static_cast<float>(Wave::Square);
        p[param::Osc1PulseWidth] = 0.3f;
        p[param::AmpAttackMs] = 20.0f;
        p[param::AmpDecayMs] = 80.0f;
        p[param::AmpSustain] = 0.6f;
        cases.push_back({"pulse_a2", p, {{0.0, 0.25, 45, 1.0f}}});
    }

    {
        // Three detuned oscillators playing a chord with overlapping notes.
        auto p = defaultValues();
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
        auto p = defaultValues();
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
        auto p = defaultValues();
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
        auto p = defaultValues();
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
        auto p = defaultValues();
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

    return cases;
}

void testGoldenRenders()
{
    const std::filesystem::path dir = SYNTHMATCH_GOLDEN_DIR;
    constexpr float tolerance = 1.0e-4f; // allows for small math-library differences between platforms

    for (const auto& c : goldenCases()) {
        RenderSettings settings;
        settings.tailSec = 0.15;
        const auto audio = render(c.params, c.notes, settings);
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
    std::filesystem::remove(path);
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
    testGoldenRenders();
    testWavFile();

    if (g_failures == 0)
        std::puts(g_updateGolden ? "Golden files updated; all other tests passed." : "All engine tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
