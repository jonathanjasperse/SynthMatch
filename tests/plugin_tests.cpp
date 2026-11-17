// Tests for the plugin's real-time tools (plugin/RealtimeTools.h): the triple
// buffer that hands playback requests to the audio thread, the output safety
// limiter, and the resampler for the reference recording. They are plain C++,
// so they run without JUCE. Plain asserts; run through CTest.

#include "RealtimeTools.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
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

constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

std::vector<float> sine(double freq, double rate, std::size_t frames, double amp = 1.0)
{
    std::vector<float> v(frames);
    for (std::size_t i = 0; i < frames; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return v;
}

// ---------------------------------------------------------------- triple buffer

void testTripleBufferOrder()
{
    rt::TripleBuffer<int> buffer;
    CHECK(!buffer.hasNew());
    CHECK(!buffer.take());
    CHECK(buffer.front() == 0);

    buffer.back() = 1;
    buffer.publish();
    CHECK(buffer.hasNew());
    CHECK(buffer.take());
    CHECK(buffer.front() == 1);
    CHECK(!buffer.hasNew());
    CHECK(!buffer.take());
    CHECK(buffer.front() == 1); // stays until something newer comes

    // The newest wins; an older one that was never taken is dropped.
    buffer.back() = 2;
    buffer.publish();
    buffer.back() = 3;
    buffer.publish();
    CHECK(buffer.take());
    CHECK(buffer.front() == 3);
    CHECK(!buffer.take());

    // The writer's slot is never the one the reader holds.
    for (int i = 0; i < 10; ++i) {
        buffer.back() = 100 + i;
        CHECK(buffer.front() != 100 + i);
        buffer.publish();
        if (i % 3 == 0) {
            CHECK(buffer.take());
            CHECK(buffer.front() == 100 + i);
        }
    }
}

// A big value that records which thread destroyed it.
std::atomic<int> g_freedOnReader{0};
std::atomic<int> g_live{0};
std::thread::id g_readerId;

struct Block {
    std::array<std::uint64_t, 32> values{};
    explicit Block(std::uint64_t v) { values.fill(v); ++g_live; }
    ~Block()
    {
        if (std::this_thread::get_id() == g_readerId)
            ++g_freedOnReader;
        --g_live;
    }
};

struct Request {
    std::uint64_t seq = 0;
    std::array<std::uint64_t, 16> copy{};
    std::shared_ptr<const Block> block;
};

void testTripleBufferThreads()
{
    // One writer publishing as fast as it can, one reader taking as fast as it
    // can. The reader must only ever see whole values, in order, and must
    // never free a block (the writer releases them).
    constexpr std::uint64_t kCount = 200000;
    auto buffer = std::make_unique<rt::TripleBuffer<Request>>();
    std::atomic<bool> readerReady{false};
    int torn = 0, backwards = 0, badBlock = 0;
    std::uint64_t seen = 0, taken = 0;

    std::thread reader([&] {
        g_readerId = std::this_thread::get_id();
        readerReady = true;
        while (seen < kCount) {
            if (!buffer->take())
                continue;
            ++taken;
            const auto& r = buffer->front();
            for (const auto v : r.copy)
                torn += v != r.seq ? 1 : 0;
            if (r.block == nullptr || r.block->values[0] != r.seq || r.block->values[31] != r.seq)
                ++badBlock;
            backwards += r.seq <= seen ? 1 : 0;
            seen = r.seq;
        }
    });
    while (!readerReady)
        std::this_thread::yield();

    for (std::uint64_t seq = 1; seq <= kCount; ++seq) {
        auto& slot = buffer->back();
        slot.seq = seq;
        slot.copy.fill(seq);
        slot.block = std::make_shared<const Block>(seq);
        buffer->publish();
        buffer->back().block.reset(); // what the processor does after sending
    }
    reader.join();

    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(badBlock == 0);
    CHECK(seen == kCount);
    CHECK(taken > 0);
    CHECK(g_freedOnReader == 0);
    buffer.reset();
    CHECK(g_live == 0);
    std::printf("triple buffer: %llu of %llu values taken, none torn\n", static_cast<unsigned long long>(taken),
                static_cast<unsigned long long>(kCount));
}

// ---------------------------------------------------------------- limiter

float peakOf(const std::vector<float>& v)
{
    float p = 0.0f;
    for (const float x : v)
        p = std::max(p, std::abs(x));
    return p;
}

void testLimiterQuietIsUntouched()
{
    // Below -3 dBFS nothing changes, to the bit.
    rt::SafetyLimiter limiter;
    limiter.prepare(48000.0);
    std::vector<float> left(48000), right(48000);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.7f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * i / 48000.0));
        right[i] = -0.5f * static_cast<float>(std::sin(2.0 * kPi * 330.0 * i / 48000.0));
    }
    auto outL = left, outR = right;
    limiter.process(outL.data(), outR.data(), static_cast<int>(outL.size()));
    CHECK(outL == left);
    CHECK(outR == right);
    CHECK(limiter.gain() == 1.0);
}

void testLimiterCeiling()
{
    // Loud chords, spikes and broken samples never get past -0.3 dBFS.
    rt::SafetyLimiter limiter;
    limiter.prepare(44100.0);
    std::uint32_t seed = 12345;
    const auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1 << 24) * 2.0f - 1.0f;
    };
    float worst = 0.0f;
    bool allFinite = true;
    for (int block = 0; block < 400; ++block) {
        const int n = 1 + block % 512;
        std::vector<float> left(static_cast<std::size_t>(n)), right(static_cast<std::size_t>(n));
        const float level = block % 7 == 0 ? 0.3f : 4.0f * std::abs(noise());
        for (int i = 0; i < n; ++i) {
            left[static_cast<std::size_t>(i)] = level * noise();
            right[static_cast<std::size_t>(i)] = level * noise();
        }
        if (block == 50)
            left[0] = std::numeric_limits<float>::quiet_NaN();
        if (block == 60)
            right[0] = std::numeric_limits<float>::infinity();
        if (block == 70)
            left[0] = 1000.0f;
        limiter.process(left.data(), right.data(), n);
        worst = std::max({worst, peakOf(left), peakOf(right)});
        for (int i = 0; i < n; ++i)
            allFinite = allFinite && std::isfinite(left[static_cast<std::size_t>(i)]) &&
                        std::isfinite(right[static_cast<std::size_t>(i)]);
    }
    CHECK(worst <= rt::SafetyLimiter::kCeiling);
    CHECK(worst > 0.9f); // it limits, it doesn't just turn things down
    CHECK(allFinite);
    std::printf("limiter: loudest output %.5f (ceiling %.5f)\n", static_cast<double>(worst),
                static_cast<double>(rt::SafetyLimiter::kCeiling));
}

void testLimiterKnee()
{
    using L = rt::SafetyLimiter;
    CHECK(L::knee(0.5f) == 0.5f);
    CHECK(L::knee(L::kThreshold) == L::kThreshold);
    float last = L::kThreshold;
    for (float p = L::kThreshold + 0.001f; p < 20.0f; p *= 1.01f) {
        const float k = L::knee(p);
        CHECK(k >= last); // never folds back
        CHECK(k <= L::kCeiling);
        CHECK(k <= p);
        last = k;
    }
    // Leaves the threshold at slope 1: a sample just above it barely changes.
    CHECK_NEAR(L::knee(L::kThreshold + 0.001f), L::kThreshold + 0.001f, 2e-5);
}

void testLimiterStereoLinkedAndRecovers()
{
    rt::SafetyLimiter limiter;
    limiter.prepare(48000.0);
    // A loud burst, the right channel at half the left.
    std::vector<float> left(4800), right(4800);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 2.0f * static_cast<float>(std::sin(2.0 * kPi * 100.0 * i / 48000.0));
        right[i] = 0.5f * left[i];
    }
    limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
    for (std::size_t i = 0; i < left.size(); ++i)
        CHECK_NEAR(right[i], 0.5f * left[i], 1e-6);
    CHECK(limiter.gain() < 0.6);

    // Then quiet again: within two seconds the gain is back to exactly 1.
    std::vector<float> quietL(96000, 0.25f), quietR(96000, -0.25f);
    limiter.process(quietL.data(), quietR.data(), static_cast<int>(quietL.size()));
    CHECK(limiter.gain() == 1.0);
    CHECK(quietL.back() == 0.25f);
    CHECK(quietR.back() == -0.25f);
    // After a spike the gain holds for 10 ms, then recovers most of the way in
    // about 100 ms.
    rt::SafetyLimiter probe;
    probe.prepare(48000.0);
    float spike[1] = {4.0f}, zero[1] = {0.0f};
    probe.process(spike, zero, 1);
    const double start = probe.gain();
    CHECK_NEAR(start, rt::SafetyLimiter::kCeiling / 4.0, 1e-3);
    std::vector<float> silentL(480, 0.0f), silentR(480, 0.0f);
    probe.process(silentL.data(), silentR.data(), 480); // 10 ms
    CHECK(probe.gain() == start);
    silentL.assign(4800, 0.0f);
    silentR.assign(4800, 0.0f);
    probe.process(silentL.data(), silentR.data(), 4800); // 100 ms
    CHECK_NEAR((probe.gain() - start) / (1.0 - start), 1.0 - std::exp(-1.0), 0.01);
}

void testLimiterSteadyToneHasNoRipple()
{
    // A steady 100 Hz tone at +6 dBFS: once limited, the hold keeps the gain
    // flat between peaks, so the tone comes out as a clean, quieter sine.
    rt::SafetyLimiter limiter;
    limiter.prepare(48000.0);
    std::vector<float> left = sine(100.0, 48000.0, 48000, 2.0), right = left;
    limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
    const double gain = limiter.gain();
    double worst = 0.0;
    for (std::size_t i = 24000; i < left.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(left[i]) - gain * 2.0 *
                                             std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / 48000.0)));
    CHECK(worst < 1e-3);
    CHECK(peakOf(left) <= rt::SafetyLimiter::kCeiling);
}

void testLimiterDeterministic()
{
    rt::SafetyLimiter a, b;
    a.prepare(96000.0);
    b.prepare(96000.0);
    std::vector<float> left(9000), right(9000);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 3.0f * static_cast<float>(std::sin(i * 0.013) * std::cos(i * 0.0007));
        right[i] = 1.5f * static_cast<float>(std::sin(i * 0.021));
    }
    auto l1 = left, r1 = right, l2 = left, r2 = right;
    a.process(l1.data(), r1.data(), 9000);
    for (int start = 0; start < 9000; start += 333) // different block sizes, same result
        b.process(l2.data() + start, r2.data() + start, std::min(333, 9000 - start));
    CHECK(l1 == l2);
    CHECK(r1 == r2);
}

// ---------------------------------------------------------------- resampler

void testResampleSameRate()
{
    const auto in = sine(440.0, 48000.0, 1000);
    std::vector<float> l, r;
    rt::resample(in.data(), in.data(), in.size(), 48000.0, 48000.0, l, r);
    CHECK(l == in);
    CHECK(r == in);
}

void testResampleUp()
{
    // 1 kHz at 44.1 kHz to 48 kHz: the same sine, sample for sample.
    const auto in = sine(1000.0, 44100.0, 44100);
    std::vector<float> l, r;
    rt::resample(in.data(), in.data(), in.size(), 44100.0, 48000.0, l, r);
    CHECK(l.size() == 48000);
    const auto ideal = sine(1000.0, 48000.0, 48000);
    double worst = 0.0;
    for (std::size_t i = 2000; i < 46000; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(l[i]) - ideal[i]));
    CHECK(worst < 1e-3);
    CHECK(l == r);
}

void testResampleDownFiltersAliases()
{
    // 96 kHz to 48 kHz: 10 kHz passes, 30 kHz (above the new Nyquist) must not
    // fold back down to 18 kHz.
    const auto pass = sine(10000.0, 96000.0, 96000);
    const auto alias = sine(30000.0, 96000.0, 96000);
    std::vector<float> l, r;
    rt::resample(pass.data(), alias.data(), pass.size(), 96000.0, 48000.0, l, r);
    CHECK(l.size() == 48000);
    CHECK_NEAR(rms(l, 2000, 46000), std::sqrt(0.5), 0.01);
    CHECK(rms(r, 2000, 46000) < 1e-3);
    std::printf("resample 96k to 48k: 10 kHz rms %.4f, 30 kHz rms %.6f\n", rms(l, 2000, 46000), rms(r, 2000, 46000));

    // Steady levels keep their gain.
    std::vector<float> dc(30000, 0.5f);
    rt::resample(dc.data(), dc.data(), dc.size(), 48000.0, 44100.0, l, r);
    for (std::size_t i = 1000; i + 1000 < l.size(); ++i)
        CHECK_NEAR(l[i], 0.5, 1e-5);
}

void testResampleOddRates()
{
    // Rates that aren't whole numbers take the blended path. It must agree
    // with the exact one, and both with the ideal sine.
    const auto in = sine(3000.0, 44100.0, 44100);
    std::vector<float> exactL, exactR, blendL, blendR;
    rt::resample(in.data(), in.data(), in.size(), 44100.0, 48000.0, exactL, exactR);
    rt::resample(in.data(), in.data(), in.size(), 44100.0, 48000.0001, blendL, blendR);
    CHECK(exactL.size() == blendL.size());
    const auto ideal = sine(3000.0, 48000.0, 48000);
    double worstExact = 0.0, worstBlend = 0.0;
    for (std::size_t i = 2000; i < 46000; ++i) {
        worstExact = std::max(worstExact, std::abs(static_cast<double>(exactL[i]) - ideal[i]));
        worstBlend = std::max(worstBlend, std::abs(static_cast<double>(blendL[i]) - ideal[i]));
    }
    CHECK(worstExact < 1e-3);
    CHECK(worstBlend < 1e-3);
    std::printf("resample 44.1k to 48k, 3 kHz: worst error %.2e exact, %.2e blended\n", worstExact, worstBlend);

    // Whole-number rates with no common factor (48001 phases, far too many
    // sets of weights) blend too.
    const auto odd = sine(500.0, 44100.0, 20000);
    rt::resample(odd.data(), odd.data(), odd.size(), 44100.0, 48001.0, exactL, exactR);
    CHECK(exactL.size() == static_cast<std::size_t>(std::llround(20000.0 * 48001.0 / 44100.0)));
    const auto oddIdeal = sine(500.0, 48001.0, exactL.size());
    double worst = 0.0;
    for (std::size_t i = 1000; i + 1000 < exactL.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(exactL[i]) - oddIdeal[i]));
    CHECK(worst < 1e-3);
}

void testResampleEnds()
{
    // Past the ends counts as silence: no garbage, and a short file works.
    std::vector<float> one(1, 1.0f), l, r;
    rt::resample(one.data(), one.data(), 1, 44100.0, 96000.0, l, r);
    CHECK(l.size() == 2);
    for (const float v : l)
        CHECK(std::isfinite(v) && std::abs(v) <= 1.1f);
    std::vector<float> empty;
    rt::resample(empty.data(), empty.data(), 0, 44100.0, 48000.0, l, r);
    CHECK(l.empty() && r.empty());
}

} // namespace

int main()
{
    testTripleBufferOrder();
    testTripleBufferThreads();
    testLimiterQuietIsUntouched();
    testLimiterCeiling();
    testLimiterKnee();
    testLimiterStereoLinkedAndRecovers();
    testLimiterSteadyToneHasNoRipple();
    testLimiterDeterministic();
    testResampleSameRate();
    testResampleUp();
    testResampleDownFiltersAliases();
    testResampleOddRates();
    testResampleEnds();

    if (g_failures == 0)
        std::puts("All plugin real-time tests passed.");
    else
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
