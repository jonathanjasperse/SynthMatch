#pragma once

// Stereo delay with feedback filters, ping-pong, a longer right channel
// (spread) and optional pitch wobble (modulation).
//
// prepare() allocates the delay lines; nothing else here allocates.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace synthmatch::dsp {

inline constexpr double kDelayMaxSeconds = 2.0;
inline constexpr double kDelayMaxSpread = 0.5;      // the right channel can be up to 50% longer
inline constexpr double kDelayMaxModSeconds = 0.005; // wobble of up to +/- 5 ms

// A dry/wet control where both are at full level in the middle: below 0.5 the
// dry signal is untouched and the wet one fades in; above 0.5 the dry fades out.
inline double dryGainForMix(double mix) { return std::min(1.0, 2.0 * (1.0 - mix)); }
inline double wetGainForMix(double mix) { return std::min(1.0, 2.0 * mix); }

class StereoDelay {
public:
    struct Settings {
        double timeSec = 0.35;
        double feedback = 0.35; // 0..0.95
        double mix = 0.25;
        bool pingPong = false;
        double spread = 0.0;    // 0..1
        double lowCutHz = 20.0;
        double highCutHz = 20000.0;
        double modRateHz = 0.5;
        double modDepth = 0.0;  // 0..1
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        const double maxSeconds = kDelayMaxSeconds * (1.0 + kDelayMaxSpread) + kDelayMaxModSeconds;
        std::size_t size = 1;
        while (static_cast<double>(size) < maxSeconds * sampleRate + 8.0)
            size <<= 1;
        for (auto& b : buffer_)
            b.assign(size, 0.0f);
        mask_ = size - 1;
        // Time changes glide over about 50 ms instead of clicking.
        glide_ = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));
        set(settings_);
        reset();
    }

    // Cheap enough for the audio thread: the lines (megabytes at high sample
    // rates) aren't cleared. Samples written before the reset read as silence.
    void reset()
    {
        written_ = 0;
        write_ = 0;
        modPhase_ = 0.0;
        lowState_[0] = lowState_[1] = highState_[0] = highState_[1] = 0.0;
        current_[0] = target_[0];
        current_[1] = target_[1];
    }

    // snap = true jumps straight to the new delay time instead of gliding.
    void set(const Settings& s, bool snap = false)
    {
        settings_ = s;
        const double seconds = std::clamp(s.timeSec, 0.001, kDelayMaxSeconds);
        target_[0] = seconds * sampleRate_;
        target_[1] = seconds * (1.0 + kDelayMaxSpread * std::clamp(s.spread, 0.0, 1.0)) * sampleRate_;
        feedback_ = std::clamp(s.feedback, 0.0, 0.95);
        dry_ = dryGainForMix(std::clamp(s.mix, 0.0, 1.0));
        wet_ = wetGainForMix(std::clamp(s.mix, 0.0, 1.0));
        lowCoeff_ = 1.0 - std::exp(-2.0 * std::numbers::pi * std::clamp(s.lowCutHz, 5.0, 0.45 * sampleRate_) / sampleRate_);
        highCoeff_ =
            1.0 - std::exp(-2.0 * std::numbers::pi * std::clamp(s.highCutHz, 100.0, 0.45 * sampleRate_) / sampleRate_);
        modIncrement_ = std::clamp(s.modRateHz, 0.0, 20.0) / sampleRate_;
        modSamples_ = std::clamp(s.modDepth, 0.0, 1.0) * kDelayMaxModSeconds * sampleRate_;
        if (snap) {
            current_[0] = target_[0];
            current_[1] = target_[1];
        }
    }

    void process(float* left, float* right, int numSamples)
    {
        if (buffer_[0].empty())
            return;

        for (int i = 0; i < numSamples; ++i) {
            current_[0] += (target_[0] - current_[0]) * glide_;
            current_[1] += (target_[1] - current_[1]) * glide_;

            double mod = 0.0;
            if (modSamples_ > 0.0) {
                mod = modSamples_ * std::sin(2.0 * std::numbers::pi * modPhase_);
                modPhase_ += modIncrement_;
                if (modPhase_ >= 1.0)
                    modPhase_ -= 1.0;
            }

            // The two channels wobble in opposite directions, which widens the echoes.
            double wet[2] = {read(0, current_[0] + mod), read(1, current_[1] - mod)};

            // Filters inside the feedback loop: each repeat gets thinner and duller.
            for (int ch = 0; ch < 2; ++ch) {
                lowState_[ch] += lowCoeff_ * (wet[ch] - lowState_[ch]);
                wet[ch] -= lowState_[ch]; // low cut
                highState_[ch] += highCoeff_ * (wet[ch] - highState_[ch]);
                wet[ch] = highState_[ch]; // high cut
            }

            const double inL = left[i], inR = right[i];
            if (settings_.pingPong) {
                // The echo bounces: input -> left line -> right line -> left line ...
                buffer_[0][write_] = static_cast<float>(0.5 * (inL + inR) + feedback_ * wet[1]);
                buffer_[1][write_] = static_cast<float>(wet[0]);
            } else {
                buffer_[0][write_] = static_cast<float>(inL + feedback_ * wet[0]);
                buffer_[1][write_] = static_cast<float>(inR + feedback_ * wet[1]);
            }
            write_ = (write_ + 1) & mask_;
            if (written_ <= mask_)
                ++written_;

            left[i] = static_cast<float>(dry_ * inL + wet_ * wet[0]);
            right[i] = static_cast<float>(dry_ * inR + wet_ * wet[1]);
        }
    }

private:
    // Reads the line `delaySamples` back from the write position, with a smooth
    // (cubic) curve through the four nearest samples.
    double read(int channel, double delaySamples) const
    {
        const double d = std::clamp(delaySamples, 3.0, static_cast<double>(mask_) - 4.0);
        const double pos = static_cast<double>(write_) - d;
        const double base = std::floor(pos);
        const double t = pos - base;
        const auto index = static_cast<std::size_t>(static_cast<long long>(base)) & mask_;

        const auto& b = buffer_[static_cast<std::size_t>(channel)];
        // How long ago x1 was written (1 = the newest sample). Anything written
        // before the last reset is silence.
        const auto age = static_cast<std::size_t>(static_cast<long long>(write_) - static_cast<long long>(base));
        const auto at = [&](std::size_t i, std::size_t sampleAge) { return sampleAge <= written_ ? b[i & mask_] : 0.0f; };
        const double x0 = at(index - 1, age + 1);
        const double x1 = at(index, age);
        const double x2 = at(index + 1, age - 1);
        const double x3 = at(index + 2, age - 2);

        const double c1 = 0.5 * (x2 - x0);
        const double c2 = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3;
        const double c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + x1;
    }

    double sampleRate_ = 48000.0;
    Settings settings_;
    std::vector<float> buffer_[2];
    std::size_t mask_ = 0;
    std::size_t write_ = 0;
    std::size_t written_ = 0; // samples written since the last reset, up to the buffer size

    double target_[2] = {0.0, 0.0};  // delay length in samples
    double current_[2] = {0.0, 0.0}; // gliding towards target_
    double glide_ = 1.0;
    double feedback_ = 0.0, dry_ = 1.0, wet_ = 0.0;
    double lowCoeff_ = 0.0, highCoeff_ = 1.0;
    double lowState_[2] = {0.0, 0.0}, highState_[2] = {0.0, 0.0};
    double modPhase_ = 0.0, modIncrement_ = 0.0, modSamples_ = 0.0;
};

} // namespace synthmatch::dsp
