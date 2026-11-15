#pragma once

// Reverb: a feedback delay network (FDN). The input is smeared by a chain of
// all-pass filters, then circulates through eight delay lines that are mixed
// into each other with a Hadamard matrix. Each line loses a little level per
// pass (the decay time) and a little extra treble (the damping).
//
// prepare() allocates the delay lines; nothing else here allocates.

#include "synthmatch/dsp/Delay.h" // dry/wet gains

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace synthmatch::dsp {

inline constexpr double kReverbMaxPredelaySeconds = 0.2;

class Reverb {
public:
    struct Settings {
        double mix = 0.25;
        double size = 0.5;         // 0..1: small room to large hall
        double decaySec = 2.0;     // time to fall 60 dB
        double damping = 0.4;      // 0..1: how much faster the treble dies
        double predelaySec = 0.01;
        double width = 1.0;        // 0 = mono, 1 = full stereo
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        const double scale = sampleRate / 48000.0;

        for (std::size_t i = 0; i < kLines; ++i)
            lines_[i].assign(static_cast<std::size_t>(kLineSamples[i] * kMaxSizeScale * scale) + 4, 0.0f);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            predelay_[ch].assign(static_cast<std::size_t>(kReverbMaxPredelaySeconds * sampleRate) + 4, 0.0f);
            for (std::size_t k = 0; k < kDiffusers; ++k) {
                diffuserLength_[ch][k] = std::max<std::size_t>(
                    1, static_cast<std::size_t>(kDiffuserSamples[ch][k] * scale));
                diffusers_[ch][k].assign(diffuserLength_[ch][k], 0.0f);
            }
        }

        set(settings_);
        reset();
    }

    void reset()
    {
        for (auto& l : lines_)
            std::fill(l.begin(), l.end(), 0.0f);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            std::fill(predelay_[ch].begin(), predelay_[ch].end(), 0.0f);
            for (auto& d : diffusers_[ch])
                std::fill(d.begin(), d.end(), 0.0f);
            diffuserPos_[ch].fill(0);
        }
        linePos_.fill(0);
        damp_.fill(0.0);
        predelayPos_ = 0;
    }

    void set(const Settings& s)
    {
        settings_ = s;
        const double scale = sampleRate_ / 48000.0;
        const double sizeScale = kMinSizeScale + (kMaxSizeScale - kMinSizeScale) * std::clamp(s.size, 0.0, 1.0);
        const double decay = std::clamp(s.decaySec, 0.05, 60.0);

        for (std::size_t i = 0; i < kLines; ++i) {
            const auto length = static_cast<std::size_t>(kLineSamples[i] * sizeScale * scale);
            lineLength_[i] = std::clamp<std::size_t>(length, 8, lines_[i].empty() ? 8 : lines_[i].size() - 1);
            // The level lost on each pass, so the tail falls 60 dB in `decay` seconds.
            lineGain_[i] = std::pow(10.0, -3.0 * static_cast<double>(lineLength_[i]) / (sampleRate_ * decay));
        }

        dampCoeff_ = 0.85 * std::clamp(s.damping, 0.0, 1.0);
        predelaySamples_ = static_cast<std::size_t>(std::clamp(s.predelaySec, 0.0, kReverbMaxPredelaySeconds) * sampleRate_);
        dry_ = dryGainForMix(std::clamp(s.mix, 0.0, 1.0));
        wet_ = wetGainForMix(std::clamp(s.mix, 0.0, 1.0));
        width_ = std::clamp(s.width, 0.0, 1.0);
    }

    void process(float* left, float* right, int numSamples)
    {
        if (lines_[0].empty())
            return;

        for (int n = 0; n < numSamples; ++n) {
            const double inL = left[n], inR = right[n];

            // Pre-delay.
            double in[2];
            for (std::size_t ch = 0; ch < 2; ++ch) {
                auto& p = predelay_[ch];
                const std::size_t size = p.size();
                in[ch] = p[(predelayPos_ + size - predelaySamples_) % size];
                p[predelayPos_] = static_cast<float>(ch == 0 ? inL : inR);
            }
            // With no pre-delay, use the input directly rather than the sample just written.
            if (predelaySamples_ == 0) {
                in[0] = inL;
                in[1] = inR;
            }
            predelayPos_ = (predelayPos_ + 1) % predelay_[0].size();

            // Diffusion: four all-pass filters per channel turn a click into a burst.
            for (std::size_t ch = 0; ch < 2; ++ch) {
                for (std::size_t k = 0; k < kDiffusers; ++k) {
                    auto& d = diffusers_[ch][k];
                    auto& pos = diffuserPos_[ch][k];
                    const double delayed = d[pos];
                    const double v = in[ch] + kDiffusion * delayed;
                    d[pos] = static_cast<float>(v);
                    in[ch] = delayed - kDiffusion * v;
                    pos = pos + 1 == diffuserLength_[ch][k] ? 0 : pos + 1;
                }
            }

            // Read the lines, with damping and decay applied.
            std::array<double, kLines> y;
            for (std::size_t i = 0; i < kLines; ++i) {
                const double out = lines_[i][linePos_[i]];
                damp_[i] = out + dampCoeff_ * (damp_[i] - out); // one-pole low-pass
                y[i] = damp_[i] * lineGain_[i];
            }

            // Mix the lines into each other (fast Hadamard transform), so energy
            // spreads evenly instead of ringing on one line.
            std::array<double, kLines> x = y;
            for (std::size_t step = 1; step < kLines; step <<= 1) {
                for (std::size_t i = 0; i < kLines; i += step << 1) {
                    for (std::size_t j = i; j < i + step; ++j) {
                        const double a = x[j], b = x[j + step];
                        x[j] = a + b;
                        x[j + step] = a - b;
                    }
                }
            }
            constexpr double kHadamardScale = 0.35355339059327373; // 1 / sqrt(8)

            for (std::size_t i = 0; i < kLines; ++i) {
                const double feed = x[i] * kHadamardScale + 0.5 * in[i & 1];
                lines_[i][linePos_[i]] = static_cast<float>(feed);
                linePos_[i] = linePos_[i] + 1 >= lineLength_[i] ? 0 : linePos_[i] + 1;
            }

            // Left hears the even lines, right the odd ones.
            const double wetL = 0.5 * (y[0] - y[2] + y[4] - y[6]);
            const double wetR = 0.5 * (y[1] - y[3] + y[5] - y[7]);
            const double mid = 0.5 * (wetL + wetR), side = 0.5 * (wetL - wetR) * width_;

            left[n] = static_cast<float>(dry_ * inL + wet_ * (mid + side));
            right[n] = static_cast<float>(dry_ * inR + wet_ * (mid - side));
        }
    }

private:
    static constexpr std::size_t kLines = 8;
    static constexpr std::size_t kDiffusers = 4;
    static constexpr double kDiffusion = 0.62;
    static constexpr double kMinSizeScale = 0.45, kMaxSizeScale = 2.2;

    // Line lengths at 48 kHz and a size scale of 1 (23 to 56 ms). Primes, so
    // their echoes don't pile up on the same instants.
    static constexpr std::array<double, kLines> kLineSamples{1123, 1327, 1511, 1733, 1949, 2203, 2437, 2671};
    static constexpr double kDiffuserSamples[2][kDiffusers] = {{229, 173, 613, 449}, {241, 167, 587, 463}};

    double sampleRate_ = 48000.0;
    Settings settings_;

    std::array<std::vector<float>, kLines> lines_;
    std::array<std::size_t, kLines> lineLength_{};
    std::array<std::size_t, kLines> linePos_{};
    std::array<double, kLines> lineGain_{};
    std::array<double, kLines> damp_{};

    std::vector<float> predelay_[2];
    std::size_t predelayPos_ = 0;
    std::size_t predelaySamples_ = 0;

    std::array<std::vector<float>, kDiffusers> diffusers_[2];
    std::array<std::size_t, kDiffusers> diffuserLength_[2]{};
    std::array<std::size_t, kDiffusers> diffuserPos_[2]{};

    double dampCoeff_ = 0.0;
    double dry_ = 1.0, wet_ = 0.0, width_ = 1.0;
};

} // namespace synthmatch::dsp
