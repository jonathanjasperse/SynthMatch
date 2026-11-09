// Engine tests. Plain asserts, no framework; run through CTest.

#include "synthmatch/Engine.h"
#include "synthmatch/Params.h"
#include "synthmatch/Render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
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

StereoBuffer renderNote(int note, double length, double tail, double sampleRate, ParamValues params = defaultValues(),
                        float velocity = 1.0f)
{
    const NoteEvent events[] = {{0.0, length, note, velocity}};
    RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.tailSec = tail;
    return render(params, events, settings);
}

void testSchema()
{
    std::set<std::string_view> ids;
    for (const auto& s : schema()) {
        CHECK(ids.insert(s.id).second); // ids are unique
        CHECK(s.min < s.max);
        CHECK(s.defaultValue >= s.min && s.defaultValue <= s.max);
        if (s.curve == Curve::Log)
            CHECK(s.min > 0.0f);

        for (const float t : {0.0f, 0.25f, 0.5f, 1.0f}) {
            const float v = fromNormalized(s, t);
            CHECK_NEAR(toNormalized(s, v), t, 1e-4);
        }
    }
    CHECK(static_cast<int>(schema().size()) == kNumParams);
    CHECK(findParam("gain_db") == param::GainDb);
    CHECK(!findParam("does_not_exist"));
}

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
    const auto a = renderNote(57, 0.5, 0.5, 48000.0);
    const auto b = renderNote(57, 0.5, 0.5, 48000.0);
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
}

void testPitch()
{
    for (const double sr : {44100.0, 48000.0, 96000.0}) {
        const auto a4 = renderNote(69, 1.0, 0.0, sr);
        CHECK_NEAR(estimateHz(a4.left, sr, at(0.1, sr), at(0.9, sr)), 440.0, 0.05);

        const auto c4 = renderNote(60, 1.0, 0.0, sr);
        CHECK_NEAR(estimateHz(c4.left, sr, at(0.1, sr), at(0.9, sr)), 261.6256, 0.05);
    }
}

void testGainAndVelocity()
{
    auto params = defaultValues();
    params[param::GainDb] = -6.0f;
    params[param::AmpAttackMs] = 1.0f;

    const double sr = 48000.0;
    const auto full = renderNote(69, 0.5, 0.0, sr, params, 1.0f);
    const float expected = std::pow(10.0f, -6.0f / 20.0f);
    CHECK_NEAR(peak(full.left, at(0.1, sr), at(0.4, sr)), expected, 0.002);

    const auto half = renderNote(69, 0.5, 0.0, sr, params, 0.5f);
    CHECK_NEAR(peak(half.left, at(0.1, sr), at(0.4, sr)), expected * 0.5, 0.002);
}

void testRelease()
{
    auto params = defaultValues();
    params[param::GainDb] = 0.0f;
    params[param::AmpAttackMs] = 1.0f;
    params[param::AmpReleaseMs] = 100.0f;

    const double sr = 48000.0;
    const auto audio = renderNote(60, 0.5, 1.0, sr, params);

    // -60 dB after the release time, and silent once the voice is freed (-80 dB).
    CHECK(peak(audio.left, at(0.5 + 0.11, sr), audio.numFrames()) < 1.0e-3f);
    CHECK(peak(audio.left, at(0.5 + 0.2, sr), audio.numFrames()) == 0.0f);
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

void testWavFile()
{
    const auto audio = renderNote(60, 0.25, 0.25, 48000.0);
    const auto path = std::filesystem::temp_directory_path() / "synthmatch_test.wav";

    std::string error;
    CHECK(writeWav(path, audio, &error));
    // 12-byte RIFF header + 26-byte fmt + 12-byte fact + 8-byte data header + samples.
    CHECK(std::filesystem::file_size(path) == 58 + audio.numFrames() * 8);
    std::filesystem::remove(path);
}

} // namespace

int main()
{
    testSchema();
    testSilenceWithoutNotes();
    testDeterminism();
    testPitch();
    testGainAndVelocity();
    testRelease();
    testVoiceStealing();
    testWavFile();

    if (g_failures == 0)
        std::puts("All engine tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
