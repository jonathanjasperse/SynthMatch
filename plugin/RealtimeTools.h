#pragma once

// Small tools for the plugin's audio thread. Plain C++ with no JUCE, so the
// tests can check them:
//   TripleBuffer   hands the newest value from the interface to the audio
//                  thread with no locks and no waiting.
//   SafetyLimiter  the last stage of the output, so overs never clip.
//   resample       converts the reference recording to the device rate, once,
//                  on the message thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace rt {

// One writer thread, one reader thread, the newest value wins. The writer fills
// back() and publishes it; the reader takes the newest published value and
// reads front() until it takes another. Each side only ever touches its own
// slot, and the slots change hands with one atomic exchange, so neither side
// can block the other. A value may hold shared_ptrs: the writer copies and
// releases them, the reader only looks at them, so nothing is ever freed on the
// reader's thread.
template <typename T>
class TripleBuffer {
public:
    // ---- Writer thread
    T& back() noexcept { return slots_[back_]; }
    void publish() noexcept
    {
        back_ = static_cast<std::uint8_t>(
            middle_.exchange(static_cast<std::uint8_t>(back_ | kFresh), std::memory_order_acq_rel) & kIndex);
    }

    // ---- Reader thread
    bool hasNew() const noexcept { return (middle_.load(std::memory_order_relaxed) & kFresh) != 0; }
    // Moves to the newest value. False (and front() unchanged) if there is none.
    bool take() noexcept
    {
        if (!hasNew())
            return false;
        front_ = static_cast<std::uint8_t>(middle_.exchange(front_, std::memory_order_acq_rel) & kIndex);
        return true;
    }
    const T& front() const noexcept { return slots_[front_]; }

private:
    static constexpr std::uint8_t kIndex = 3, kFresh = 4;
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

    std::array<T, 3> slots_{};
    std::atomic<std::uint8_t> middle_{1};
    std::uint8_t back_ = 0;  // writer only
    std::uint8_t front_ = 2; // reader only
};

// Keeps every output sample within -0.3 dBFS, so overs never clip at the audio
// device. Below -3 dBFS it changes nothing. Above, a soft knee bends peaks
// toward the ceiling. The gain drops at once for a loud sample (worked out from
// that sample, so there is no lookahead and no latency), holds for 10 ms so a
// steady loud tone isn't rippled between its peaks, then recovers over about
// 100 ms. Both channels share one gain, so the stereo image holds. A NaN or Inf
// sample comes out as silence. No allocation; deterministic.
class SafetyLimiter {
public:
    static constexpr float kThreshold = 0.70794578f; // -3 dBFS
    static constexpr float kCeiling = 0.96605088f;   // -0.3 dBFS
    static constexpr double kHoldSec = 0.01;
    static constexpr double kReleaseSec = 0.1;

    void prepare(double sampleRate) noexcept
    {
        const double rate = std::max(1.0, sampleRate);
        holdSamples_ = static_cast<int>(kHoldSec * rate);
        release_ = std::exp(-1.0 / (kReleaseSec * rate));
        reset();
    }
    void reset() noexcept
    {
        gain_ = 1.0;
        holdLeft_ = 0;
    }
    double gain() const noexcept { return gain_; }

    // The output peak for an input peak: the same below the threshold, then a
    // curve that leaves it at slope 1 and approaches the ceiling.
    static float knee(float peak) noexcept
    {
        if (peak <= kThreshold)
            return peak;
        constexpr float room = kCeiling - kThreshold;
        return kThreshold + room * std::tanh((peak - kThreshold) / room);
    }

    void process(float* left, float* right, int numSamples) noexcept
    {
        double g = gain_;
        int holdLeft = holdLeft_;
        for (int i = 0; i < numSamples; ++i) {
            const float l = finiteOrZero(left[i]);
            const float r = finiteOrZero(right[i]);
            const float peak = std::max(std::abs(l), std::abs(r));
            const double target = peak > kThreshold ? static_cast<double>(knee(peak)) / peak : 1.0;
            if (target <= g) {
                g = target; // instant: this sample already fits under the ceiling
                holdLeft = holdSamples_;
            } else if (holdLeft > 0) {
                --holdLeft;
            } else {
                g = target - (target - g) * release_;
                if (target == 1.0 && g > 1.0 - 1e-6)
                    g = 1.0; // recovered (a -120 dB step): pass samples through untouched
            }
            const auto gf = static_cast<float>(g);
            // The clamp only catches rounding: the gain already keeps peaks below.
            left[i] = std::clamp(l * gf, -kCeiling, kCeiling);
            right[i] = std::clamp(r * gf, -kCeiling, kCeiling);
        }
        gain_ = g;
        holdLeft_ = holdLeft;
    }

private:
    // Checks the bits, so fast-math settings can't optimise the test away.
    static float finiteOrZero(float x) noexcept
    {
        return (std::bit_cast<std::uint32_t>(x) & 0x7f800000u) == 0x7f800000u ? 0.0f : x;
    }

    double gain_ = 1.0, release_ = 0.0;
    int holdSamples_ = 0, holdLeft_ = 0;
};


// Converts a stereo recording from one sample rate to another with a
// windowed-sinc low-pass (Kaiser window, about 80 dB of rejection), so nothing
// above the lower of the two Nyquist frequencies folds back. Equal rates copy.
// Each output sample is a weighted sum of the inputs around its position. With
// whole-number rates (44100 to 48000 and the like) the positions repeat every
// few hundred samples, so each set of weights is worked out once; other rates
// blend the two nearest of 1024 sets. Too slow for the audio thread: call it
// once, on the message thread.
inline void resample(const float* inLeft, const float* inRight, std::size_t numFrames, double fromRate, double toRate,
                     std::vector<float>& outLeft, std::vector<float>& outRight)
{
    if (fromRate == toRate || numFrames == 0 || !(fromRate > 0.0) || !(toRate > 0.0)) {
        outLeft.assign(inLeft, inLeft + numFrames);
        outRight.assign(inRight, inRight + numFrames);
        return;
    }

    const double ratio = toRate / fromRate;
    const auto outFrames = static_cast<std::size_t>(std::llround(static_cast<double>(numFrames) * ratio));
    outLeft.assign(outFrames, 0.0f);
    outRight.assign(outFrames, 0.0f);

    // The low-pass, with its cutoff just under the lower Nyquist (in cycles per
    // input sample) and 24 zero crossings each side. Output n sits at input
    // position x = n * fromRate / toRate and takes the inputs from
    // floor(x) - reach + 1 to floor(x) + reach.
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kZeroCrossings = 24.0;
    constexpr double kBeta = 7.86; // Kaiser beta for about 80 dB
    const double cutoff = 0.5 * std::min(1.0, ratio) * 0.95;
    const double halfWidth = kZeroCrossings / (2.0 * cutoff); // input samples
    const auto reach = static_cast<std::int64_t>(std::ceil(halfWidth));
    const auto taps = static_cast<std::size_t>(2 * reach);

    const auto besselI0 = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            const double t = x / (2.0 * k);
            term *= t * t;
            sum += term;
            if (term < sum * 1e-17)
                break;
        }
        return sum;
    };
    const double i0Beta = besselI0(kBeta);
    const auto kernel = [&](double distance) {
        const double u = distance / halfWidth;
        if (u <= -1.0 || u >= 1.0)
            return 0.0;
        const double x = 2.0 * cutoff * distance;
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        return sinc * besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0Beta;
    };

    // Phases are the fractions of an input sample an output can sit at.
    // Whole-number rates hit exactly `to / gcd` of them; anything else blends
    // the two nearest of 1024.
    constexpr std::int64_t kBlendPhases = 1024;
    constexpr std::int64_t kMaxWeights = std::int64_t{1} << 20;
    std::int64_t phases = kBlendPhases, step = 0;
    bool exact = false;
    if (fromRate == std::floor(fromRate) && toRate == std::floor(toRate) && fromRate < 1e7 && toRate < 1e7) {
        const auto from = static_cast<std::int64_t>(fromRate);
        const auto to = static_cast<std::int64_t>(toRate);
        const auto g = std::gcd(from, to);
        if ((to / g) * static_cast<std::int64_t>(taps) <= kMaxWeights) {
            phases = to / g;
            step = from / g;
            exact = true;
        }
    }

    // One set of weights per phase, plus one at a whole sample for blending.
    // Each set adds up to 1, so a steady level keeps its gain at every phase.
    std::vector<float> weights((static_cast<std::size_t>(phases) + 1) * taps);
    std::vector<double> set(taps);
    for (std::int64_t p = 0; p <= phases; ++p) {
        const double frac = static_cast<double>(p) / static_cast<double>(phases);
        double sum = 0.0;
        for (std::size_t j = 0; j < taps; ++j) {
            set[j] = kernel(static_cast<double>(static_cast<std::int64_t>(j) - reach + 1) - frac);
            sum += set[j];
        }
        float* w = weights.data() + static_cast<std::size_t>(p) * taps;
        for (std::size_t j = 0; j < taps; ++j)
            w[j] = static_cast<float>(set[j] / sum);
    }

    // The weighted sum of the inputs from `first` on; past the ends is silence.
    const auto frames = static_cast<std::int64_t>(numFrames);
    const auto weigh = [&](std::int64_t phase, std::int64_t first, float& l, float& r) {
        const float* w = weights.data() + static_cast<std::size_t>(phase) * taps;
        float sumL = 0.0f, sumR = 0.0f;
        if (first >= 0 && first + static_cast<std::int64_t>(taps) <= frames) {
            const float* a = inLeft + first;
            const float* b = inRight + first;
            for (std::size_t j = 0; j < taps; ++j) {
                sumL += w[j] * a[j];
                sumR += w[j] * b[j];
            }
        } else {
            const auto from = static_cast<std::size_t>(std::max<std::int64_t>(0, -first));
            const auto to = static_cast<std::size_t>(std::clamp<std::int64_t>(frames - first, 0, static_cast<std::int64_t>(taps)));
            for (std::size_t j = from; j < to; ++j) {
                sumL += w[j] * inLeft[first + static_cast<std::int64_t>(j)];
                sumR += w[j] * inRight[first + static_cast<std::int64_t>(j)];
            }
        }
        l = sumL;
        r = sumR;
    };

    for (std::size_t n = 0; n < outFrames; ++n) {
        std::int64_t base = 0, phase = 0;
        float blend = 0.0f;
        if (exact) {
            const auto position = static_cast<std::int64_t>(n) * step; // in 1/phases of an input sample
            base = position / phases;
            phase = position % phases;
        } else {
            const double x = static_cast<double>(n) * fromRate / toRate;
            base = static_cast<std::int64_t>(std::floor(x));
            const double f = (x - static_cast<double>(base)) * static_cast<double>(phases);
            phase = std::clamp<std::int64_t>(static_cast<std::int64_t>(f), 0, phases - 1);
            blend = static_cast<float>(f - static_cast<double>(phase));
        }
        const std::int64_t first = base - reach + 1;
        float l = 0.0f, r = 0.0f;
        weigh(phase, first, l, r);
        if (blend > 0.0f) {
            float l2 = 0.0f, r2 = 0.0f;
            weigh(phase + 1, first, l2, r2);
            l += blend * (l2 - l);
            r += blend * (r2 - r);
        }
        outLeft[n] = l;
        outRight[n] = r;
    }
}

} // namespace rt
