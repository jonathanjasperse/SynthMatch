#pragma once

// Reverb: a feedback delay network (FDN).
//
//   input -> pre-delay -> low and high cut -> four all-passes (diffusion)
//         -> early reflections, read from the smeared input
//         -> eight delay lines, mixed into each other with a Hadamard matrix
//
// Each line reads with a slow, seeded random wobble, so the network's fixed
// resonances smear out instead of ringing. It loses level on each pass in
// three bands (bass, mid and treble decay times are set in seconds, so the
// decay doesn't change with the size or the sample rate) and goes through an
// all-pass of its own for a denser build-up. The mode sets the line lengths,
// diffusion, early reflections and colour: room, chamber, hall, plate or vintage.
//
// While playing, size, pre-delay and the other settings glide to new values
// instead of jumping, so a ringing tail doesn't click, and a mode change fades
// the tail out and starts the new mode empty.
//
// prepare() allocates; nothing else here does. reset() clears nothing big:
// samples written before it are read as silence.

#include "synthmatch/dsp/Delay.h" // dry/wet gains

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace synthmatch::dsp {

inline constexpr double kReverbMaxPredelaySeconds = 0.2;

// Same order as the reverb_mode parameter's choices.
enum class ReverbMode { Room, Chamber, Hall, Plate, Vintage, Count };

namespace reverb {

struct Tap {
    double ms, left, right;
};

// The structure of one mode. Lengths are in samples at 48 kHz.
struct Shape {
    std::array<double, 8> lines;        // the delay lines at a size scale of 1 (primes)
    std::array<double, 8> allpass;      // the all-pass inside each line
    double minScale, maxScale;          // the line scale at size 0 and at size 1
    double inDiffusion, lateDiffusion;  // all-pass gains at the default diffusion (0.75)
    double earlyLevel;                  // early reflections, 0 = none
    std::array<Tap, 8> early;           // at a size scale of 1
    double modMs;                       // peak wobble at mod depth 1
    double modRate;                     // times the mod rate setting
    double bassHz;                      // below here the bass decay time applies
    double bassMult, trebleMult;        // the mode's own voicing of the bass and treble decay
    double colourHz;                    // a two-pole low-pass on the output, 0 = none
    double direct;                      // the smeared input in the output, for an instant start
    double trimDb;                      // so that every mode is about as loud
};

// Mixed signs, so the taps don't all add up at low frequencies and boom on some notes.
inline constexpr std::array<Tap, 8> kRoomTaps{{{4.1, 0.9, -0.7}, {6.7, -0.6, 0.9}, {9.3, 0.7, 0.5}, {12.9, -0.5, -0.6},
                                               {16.1, 0.45, -0.35}, {21.7, -0.3, 0.4}, {27.3, 0.25, 0.2}, {33.9, -0.15, -0.2}}};
inline constexpr std::array<Tap, 8> kChamberTaps{{{7.3, 0.7, -0.5}, {11.1, -0.5, 0.7}, {15.7, 0.5, 0.4}, {19.3, -0.35, -0.45},
                                                  {25.9, 0.3, -0.25}, {31.1, -0.2, 0.25}, {39.7, 0.15, 0.12}, {47.3, -0.1, -0.12}}};
inline constexpr std::array<Tap, 8> kHallTaps{{{13.1, 0.5, -0.3}, {19.7, -0.3, 0.5}, {27.1, 0.35, 0.25}, {33.7, -0.25, -0.3},
                                               {44.3, 0.2, -0.15}, {53.9, -0.12, 0.16}, {67.1, 0.1, 0.08}, {79.3, -0.06, -0.08}}};

inline constexpr std::array<double, 8> kHallLines{1123, 1327, 1511, 1733, 1949, 2203, 2437, 2671};
inline constexpr std::array<double, 8> kHallAllpass{149, 211, 263, 293, 337, 379, 419, 461};

inline constexpr std::array<Shape, static_cast<std::size_t>(ReverbMode::Count)> kShapes{{
    // Room: short lines and strong, early reflections. The bass dies a little sooner, so it doesn't boom.
    {{467, 547, 619, 709, 787, 877, 953, 1039}, {101, 127, 149, 173, 191, 211, 233, 257}, 0.6, 1.8, 0.62, 0.45, 0.45,
     kRoomTaps, 0.8, 1.0, 200.0, 0.8, 1.0, 0.0, 0.0, -3.9},
    // Chamber: a bigger, livelier room.
    {{709, 811, 919, 1031, 1129, 1237, 1361, 1471}, {131, 157, 181, 199, 223, 241, 263, 281}, 0.6, 1.8, 0.7, 0.55, 0.45,
     kChamberTaps, 1.0, 1.0, 220.0, 0.85, 1.0, 0.0, 0.0, -3.1},
    // Hall: long lines, sparse and late reflections.
    {kHallLines, kHallAllpass, 0.45, 2.2, 0.62, 0.5, 0.4, kHallTaps, 1.0, 1.0, 250.0, 1.0, 1.0, 0.0, 0.0, 1.0},
    // Plate: no reflections, dense and bright from the very start.
    {{887, 1031, 1187, 1307, 1433, 1609, 1747, 1913}, {113, 139, 167, 197, 227, 251, 277, 307}, 0.6, 1.6, 0.75, 0.65, 0.0,
     {}, 1.0, 1.0, 150.0, 1.0, 1.2, 0.0, 0.3, -1.8},
    // Vintage: an old digital hall. Darker, a little grainier, with a slower,
    // deeper chorus in the tail and a bass that hangs on.
    {kHallLines, kHallAllpass, 0.45, 2.2, 0.58, 0.42, 0.25, kHallTaps, 2.4, 0.7, 300.0, 1.3, 0.75, 7500.0, 0.0, 1.0},
}};

} // namespace reverb

class Reverb {
public:
    struct Settings {
        ReverbMode mode = ReverbMode::Hall;
        double mix = 0.15;
        double size = 0.5;         // 0..1: small to large, within the mode's range
        double decaySec = 1.8;     // time for the mids to fall 60 dB
        double damping = 0.4;      // 0..1: how much faster the treble dies
        double predelaySec = 0.02;
        double width = 1.0;        // 0 = mono, 1 = full stereo
        double modDepth = 0.35;    // 0..1: how far the lines wobble
        double modRateHz = 0.5;
        double diffusion = 0.75;   // 0..1: how quickly the echoes blur into a wash
        double bassMult = 1.0;     // the bass decay time, as a multiple of decaySec
        double lowCutHz = 20.0;    // on what goes into the reverb
        double highCutHz = 20000.0;
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        const double scale = sampleRate / 48000.0;
        double maxModMs = 0.0, maxEarlySec = 0.0;
        for (const auto& s : reverb::kShapes) {
            maxModMs = std::max(maxModMs, s.modMs);
            for (const auto& t : s.early)
                maxEarlySec = std::max(maxEarlySec, 0.001 * t.ms * s.maxScale);
        }
        // Room for the wobble: the smoothed random curve can overshoot its points by a quarter.
        const double modRoom = 1.3 * 0.001 * maxModMs * sampleRate + 8.0;
        for (std::size_t i = 0; i < kLines; ++i) {
            double longest = 0.0, longestAllpass = 0.0;
            for (const auto& s : reverb::kShapes) {
                longest = std::max(longest, s.lines[i] * s.maxScale);
                longestAllpass = std::max(longestAllpass, s.allpass[i]);
            }
            const std::size_t n = powerOfTwo(longest * scale + modRoom);
            lines_[i].assign(n, 0.0f);
            lineMask_[i] = n - 1;
            allpass_[i].assign(static_cast<std::size_t>(longestAllpass * scale) + 2, 0.0f);
        }
        const std::size_t inputSize = powerOfTwo(kReverbMaxPredelaySeconds * sampleRate + 8.0);
        const std::size_t earlySize = powerOfTwo(maxEarlySec * sampleRate + 8.0);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            input_[ch].assign(inputSize, 0.0f);
            early_[ch].assign(earlySize, 0.0f);
            for (std::size_t k = 0; k < kDiffusers; ++k) {
                diffuserLength_[ch][k] = std::max<std::size_t>(1, static_cast<std::size_t>(kDiffuserSamples[ch][k] * scale));
                diffusers_[ch][k].assign(diffuserLength_[ch][k], 0.0f);
            }
        }
        inputMask_ = inputSize - 1;
        earlyMask_ = earlySize - 1;

        const auto rate = [&](double seconds, double every) { return 1.0 - std::exp(-every / (seconds * sampleRate)); };
        sizeGlide_ = rate(0.08, kControl);    // two stages of 80 ms: a change takes about a quarter of a second
        controlGlide_ = rate(0.05, kControl); // decay, diffusion and wobble depth: about 50 ms
        predelayGlide_ = rate(0.03, 1.0);     // two stages of 30 ms
        mixGlide_ = rate(0.02, 1.0);          // about 20 ms
        modeFadeStep_ = 1.0 / (0.01 * sampleRate); // 10 ms
        reset();
    }

    // Silences the tail and starts again with the current settings. Cheap
    // enough for the audio thread: nothing big is cleared.
    void reset()
    {
        if (!prepared())
            return;
        count_ = 0;
        for (auto& p : diffuserPos_)
            p.fill(0);
        allpassPos_.fill(0);
        bass_.fill(0.0);
        treble_.fill(0.0);
        for (std::size_t ch = 0; ch < 2; ++ch)
            lowCut_[ch] = highCut_[ch] = colour_[ch] = {0.0, 0.0};

        mode_ = settings_.mode;
        switching_ = false;
        modeGain_ = 1.0;

        // Seeded wobble: the same tail every time.
        rng_ = 0x2545F491u;
        for (std::size_t i = 0; i < kLines; ++i) {
            for (auto& point : modPoints_[i])
                point = noise();
            modPhase_[i] = static_cast<double>(i) / static_cast<double>(kLines);
        }
        controlCount_ = 0;
        apply(true);
    }

    // snap = true jumps straight to the new settings instead of gliding, for
    // setting up before playing.
    void set(const Settings& s, bool snap = false)
    {
        settings_ = s;
        if (!prepared())
            return;
        if (s.mode != mode_ && snap) {
            reset(); // takes the new mode
            return;
        }
        // While playing, a new mode waits until the old one has faded out (see process()).
        switching_ = s.mode != mode_;
        apply(snap);
    }

    void process(float* left, float* right, int numSamples)
    {
        if (!prepared())
            return;

        for (int n = 0; n < numSamples; ++n) {
            if (controlCount_ == 0)
                control();
            controlCount_ = controlCount_ + 1 == kControl ? 0 : controlCount_ + 1;

            // A mode change: fade out, switch, fade back in.
            if (switching_) {
                modeGain_ -= modeFadeStep_;
                if (modeGain_ <= 0.0) {
                    modeGain_ = 0.0;
                    switchMode();
                }
            } else if (modeGain_ < 1.0) {
                modeGain_ = std::min(1.0, modeGain_ + modeFadeStep_);
            }

            const double inL = left[n], inR = right[n];

            // Pre-delay, gliding (smoothly, in two stages) when it changes.
            input_[0][pos_ & inputMask_] = static_cast<float>(inL);
            input_[1][pos_ & inputMask_] = static_cast<float>(inR);
            if (predelay_ != predelayTarget_)
                glide(predelayHalf_, predelay_, predelayTarget_, predelayGlide_, 1.0e-3);
            double in[2] = {readSmooth(input_[0], inputMask_, predelay_), readSmooth(input_[1], inputMask_, predelay_)};

            for (std::size_t ch = 0; ch < 2; ++ch) {
                // The tone of what goes in: low and high cut, 12 dB per octave each.
                double v = in[ch];
                for (auto& state : lowCut_[ch]) {
                    state += lowCutCoeff_ * (v - state);
                    v -= state;
                }
                for (auto& state : highCut_[ch]) {
                    state = highCutCoeff_ >= 1.0 ? v : state + highCutCoeff_ * (v - state); // 1 = off
                    v = state;
                }
                // Diffusion: four all-passes turn a click into a burst.
                for (std::size_t k = 0; k < kDiffusers; ++k) {
                    auto& d = diffusers_[ch][k];
                    auto& at = diffuserPos_[ch][k];
                    const double delayed = count_ >= diffuserLength_[ch][k] ? d[at] : 0.0;
                    const double w = v + inGain_ * delayed;
                    d[at] = static_cast<float>(w);
                    v = delayed - inGain_ * w;
                    at = at + 1 == diffuserLength_[ch][k] ? 0 : at + 1;
                }
                in[ch] = v;
            }

            // Early reflections: taps on the smeared input, so each is a short burst, not a click.
            early_[0][pos_ & earlyMask_] = static_cast<float>(in[0]);
            early_[1][pos_ & earlyMask_] = static_cast<float>(in[1]);
            double earlyL = 0.0, earlyR = 0.0;
            if (earlyOn_) {
                for (std::size_t k = 0; k < kTaps; ++k) {
                    earlyL += earlyLeft_[k] * readSmooth(early_[0], earlyMask_, earlyAge_[k]);
                    earlyR += earlyRight_[k] * readSmooth(early_[1], earlyMask_, earlyAge_[k]);
                }
            }

            // The lines: read with the wobble, lose level in three bands, then an all-pass.
            std::array<double, kLines> y;
            for (std::size_t i = 0; i < kLines; ++i) {
                delay_[i] += delayStep_[i];
                double v = readLine(i, delay_[i]);
                bass_[i] += bassCoeff_ * (v - bass_[i]);
                treble_[i] += trebleCoeff_ * (v - treble_[i]); // everything below the treble
                v = gainLow_[i] * bass_[i] + gainMid_[i] * (treble_[i] - bass_[i]) + gainHigh_[i] * (v - treble_[i]);

                auto& a = allpass_[i];
                auto& at = allpassPos_[i];
                const double delayed = count_ >= allpassLength_[i] ? a[at] : 0.0;
                const double w = v + lateGain_ * delayed;
                a[at] = static_cast<float>(w);
                y[i] = delayed - lateGain_ * w;
                at = at + 1 >= allpassLength_[i] ? 0 : at + 1;
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
            for (std::size_t i = 0; i < kLines; ++i)
                lines_[i][pos_ & lineMask_[i]] = static_cast<float>(x[i] * kHadamardScale + 0.5 * in[i & 1]);

            // Left hears the even lines, right the odd ones.
            double wet[2] = {0.5 * (y[0] - y[2] + y[4] - y[6]) + earlyL + direct_ * in[0],
                             0.5 * (y[1] - y[3] + y[5] - y[7]) + earlyR + direct_ * in[1]};
            if (colourCoeff_ < 1.0) {
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    for (auto& state : colour_[ch]) {
                        state += colourCoeff_ * (wet[ch] - state);
                        wet[ch] = state;
                    }
                }
            }

            dry_ += mixGlide_ * (dryTarget_ - dry_);
            wet_ += mixGlide_ * (wetTarget_ - wet_);
            width_ += mixGlide_ * (widthTarget_ - width_);
            const double gain = wet_ * trim_ * modeGain_;
            const double mid = 0.5 * (wet[0] + wet[1]), side = 0.5 * (wet[0] - wet[1]) * width_;
            left[n] = static_cast<float>(dry_ * inL + gain * (mid + side));
            right[n] = static_cast<float>(dry_ * inR + gain * (mid - side));

            ++pos_;
            if (count_ < kCountLimit)
                ++count_;
        }
    }

private:
    static constexpr std::size_t kLines = 8;
    static constexpr std::size_t kDiffusers = 4;
    static constexpr std::size_t kTaps = 8;
    static constexpr int kControl = 32;                        // samples between wobble and glide updates
    static constexpr std::size_t kCountLimit = std::size_t{1} << 30;
    static constexpr double kDiffuserSamples[2][kDiffusers] = {{229, 173, 613, 449}, {241, 167, 587, 463}};

    bool prepared() const { return !lines_[0].empty(); }

    const reverb::Shape& shape() const
    {
        const auto m = static_cast<std::size_t>(mode_);
        return reverb::kShapes[m < reverb::kShapes.size() ? m : static_cast<std::size_t>(ReverbMode::Hall)];
    }

    static std::size_t powerOfTwo(double atLeast)
    {
        std::size_t n = 1;
        while (static_cast<double>(n) < atLeast)
            n <<= 1;
        return n;
    }

    double onePole(double hz) const
    {
        return 1.0 - std::exp(-2.0 * std::numbers::pi * std::clamp(hz, 5.0, 0.45 * sampleRate_) / sampleRate_);
    }

    // Two one-pole stages: the value eases into and out of a change instead of
    // jumping into motion, which would put a kink in a ringing tail.
    static void glide(double& half, double& value, double target, double amount, double close)
    {
        half += amount * (target - half);
        value += amount * (half - value);
        if (std::abs(target - value) <= close && std::abs(target - half) <= close)
            half = value = target;
    }

    // Works out everything that follows from the settings and the active mode.
    void apply(bool snap)
    {
        const auto& sh = shape();
        const auto& s = settings_;
        const double scale = sampleRate_ / 48000.0;

        scaleTarget_ = sh.minScale + (sh.maxScale - sh.minScale) * std::clamp(s.size, 0.0, 1.0);
        const double decay = std::clamp(s.decaySec, 0.05, 60.0);
        const double damping = std::clamp(s.damping, 0.0, 1.0);
        const double trebleDecay = decay * sh.trebleMult * std::pow(10.0, -damping);
        const double bassDecay = decay * sh.bassMult * std::clamp(s.bassMult, 0.25, 4.0);
        for (std::size_t i = 0; i < kLines; ++i) {
            allpassLength_[i] = std::clamp<std::size_t>(static_cast<std::size_t>(sh.allpass[i] * scale), 1, allpass_[i].size());
            if (allpassPos_[i] >= allpassLength_[i])
                allpassPos_[i] = 0;
            // The level lost on each pass, per band, so the tail falls 60 dB in
            // each band's decay time whatever the line's length.
            const double loopSec = (sh.lines[i] * scaleTarget_ * scale + static_cast<double>(allpassLength_[i])) / sampleRate_;
            gainMidTarget_[i] = std::pow(10.0, -3.0 * loopSec / decay);
            gainHighTarget_[i] = std::pow(10.0, -3.0 * loopSec / trebleDecay);
            gainLowTarget_[i] = std::pow(10.0, -3.0 * loopSec / bassDecay);
        }
        bassCoeff_ = onePole(sh.bassHz);
        trebleCoeff_ = onePole(8000.0 * std::pow(1500.0 / 8000.0, damping));
        // Two stages each, placed so the cut is 3 dB down at the frequency set.
        constexpr double kTwoStage = 0.6436; // sqrt(sqrt(2) - 1)
        lowCutCoeff_ = onePole(s.lowCutHz * kTwoStage);
        highCutCoeff_ = s.highCutHz >= 19999.0 ? 1.0 : onePole(s.highCutHz / kTwoStage);
        colourCoeff_ = sh.colourHz > 0.0 ? onePole(sh.colourHz) : 1.0;

        modSamplesTarget_ = std::clamp(s.modDepth, 0.0, 1.0) * sh.modMs * 0.001 * sampleRate_;
        modStep_ = std::clamp(s.modRateHz, 0.01, 20.0) * sh.modRate * kControl / sampleRate_;

        const double diffusion = std::clamp(s.diffusion, 0.0, 1.0) / 0.75;
        inGainTarget_ = std::min(0.8, sh.inDiffusion * diffusion);
        lateGainTarget_ = std::min(0.7, sh.lateDiffusion * diffusion);

        earlyOn_ = sh.earlyLevel > 0.0;
        for (std::size_t k = 0; k < kTaps; ++k) {
            earlyLeft_[k] = sh.earlyLevel * sh.early[k].left;
            earlyRight_[k] = sh.earlyLevel * sh.early[k].right;
        }
        direct_ = sh.direct;
        trim_ = std::pow(10.0, sh.trimDb / 20.0);

        predelayTarget_ = std::clamp(s.predelaySec, 0.0, kReverbMaxPredelaySeconds) * sampleRate_;
        const double mix = std::clamp(s.mix, 0.0, 1.0);
        dryTarget_ = dryGainForMix(mix);
        wetTarget_ = wetGainForMix(mix);
        widthTarget_ = std::clamp(s.width, 0.0, 1.0);

        if (snap) {
            scale_ = scaleHalf_ = scaleTarget_;
            predelay_ = predelayHalf_ = predelayTarget_;
            dry_ = dryTarget_;
            wet_ = wetTarget_;
            width_ = widthTarget_;
            gainLow_ = gainLowTarget_;
            gainMid_ = gainMidTarget_;
            gainHigh_ = gainHighTarget_;
            inGain_ = inGainTarget_;
            lateGain_ = lateGainTarget_;
            modSamples_ = modSamplesTarget_;
            for (std::size_t i = 0; i < kLines; ++i) {
                delay_[i] = lineDelay(i);
                delayStep_[i] = 0.0;
            }
            updateEarly();
        }
    }

    // The old mode has faded out: start the new one, empty.
    void switchMode()
    {
        const double dry = dry_, wet = wet_, width = width_;
        reset();
        dry_ = dry;
        wet_ = wet;
        width_ = width;
        modeGain_ = 0.0; // and fade in
    }

    // Every kControl samples: the settings glide a step, and each line gets
    // the next point of its wobble, which it ramps to over the next kControl samples.
    void control()
    {
        const auto towards = [](double& value, double target, double amount) {
            if (value == target)
                return;
            value += amount * (target - value);
            if (std::abs(target - value) <= 1.0e-9 * std::max(1.0, std::abs(target)))
                value = target;
        };
        if (scale_ != scaleTarget_)
            glide(scaleHalf_, scale_, scaleTarget_, sizeGlide_, 1.0e-9);
        towards(inGain_, inGainTarget_, controlGlide_);
        towards(lateGain_, lateGainTarget_, controlGlide_);
        towards(modSamples_, modSamplesTarget_, controlGlide_);
        for (std::size_t i = 0; i < kLines; ++i) {
            towards(gainLow_[i], gainLowTarget_[i], controlGlide_);
            towards(gainMid_[i], gainMidTarget_[i], controlGlide_);
            towards(gainHigh_[i], gainHighTarget_[i], controlGlide_);

            modPhase_[i] += modStep_ * (0.8 + 0.05 * static_cast<double>(i)); // each line at its own pace
            while (modPhase_[i] >= 1.0) {
                modPhase_[i] -= 1.0;
                auto& p = modPoints_[i];
                p = {p[1], p[2], p[3], noise()};
            }
            delayStep_[i] = (lineDelay(i) - delay_[i]) / kControl;
        }
        updateEarly();
    }

    // A line's length now, with its wobble, in samples.
    double lineDelay(std::size_t i) const
    {
        const auto& p = modPoints_[i];
        const double t = modPhase_[i];
        // A smooth (Catmull-Rom) curve through the random points.
        const double wobble = 0.5 * (2.0 * p[1] + (p[2] - p[0]) * t + (2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3]) * t * t +
                                     (3.0 * (p[1] - p[2]) + p[3] - p[0]) * t * t * t);
        return shape().lines[i] * scale_ * (sampleRate_ / 48000.0) + modSamples_ * wobble;
    }

    void updateEarly()
    {
        const auto& sh = shape();
        const double limit = static_cast<double>(earlyMask_) - 2.0;
        for (std::size_t k = 0; k < kTaps; ++k)
            earlyAge_[k] = std::min(limit, 0.001 * sh.early[k].ms * scale_ * sampleRate_);
    }

    // xorshift32: white noise in [-1, 1).
    double noise()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<double>(static_cast<std::int32_t>(rng_)) * (1.0 / 2147483648.0);
    }

    // A sample `age` samples old (0 = the one just written) from a buffer
    // written at pos_. Samples from before the last reset are silence.
    float sampleAt(const std::vector<float>& buffer, std::size_t mask, std::size_t age) const
    {
        return age <= count_ ? buffer[(pos_ - age) & mask] : 0.0f;
    }

    // Linear interpolation, `age` samples back.
    double readSmooth(const std::vector<float>& buffer, std::size_t mask, double age) const
    {
        const double whole = std::floor(age);
        const auto k = static_cast<std::size_t>(whole);
        const double t = age - whole;
        const double a = sampleAt(buffer, mask, k);
        return t == 0.0 ? a : a + t * (sampleAt(buffer, mask, k + 1) - a);
    }

    // Cubic interpolation, `delay` samples back from the next write.
    double readLine(std::size_t i, double delay) const
    {
        const auto& l = lines_[i];
        const auto mask = lineMask_[i];
        const double d = std::clamp(delay, 2.0, static_cast<double>(mask) - 3.0);
        const double whole = std::floor(d);
        const auto k = static_cast<std::size_t>(whole);
        const double t = d - whole;
        const double x0 = sampleAt(l, mask, k - 1), x1 = sampleAt(l, mask, k), x2 = sampleAt(l, mask, k + 1),
                     x3 = sampleAt(l, mask, k + 2);
        const double c1 = 0.5 * (x2 - x0);
        const double c2 = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3;
        const double c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + x1;
    }

    double sampleRate_ = 48000.0;
    Settings settings_;
    ReverbMode mode_ = ReverbMode::Hall; // the mode playing now
    bool switching_ = false;             // a new mode is waiting for this one to fade out
    double modeGain_ = 1.0, modeFadeStep_ = 0.0;

    // Buffers. Every one is written at pos_ (masked), once per sample.
    std::array<std::vector<float>, kLines> lines_;
    std::array<std::size_t, kLines> lineMask_{};
    std::array<std::vector<float>, kLines> allpass_;
    std::array<std::size_t, kLines> allpassLength_{}, allpassPos_{};
    std::vector<float> input_[2], early_[2];
    std::size_t inputMask_ = 0, earlyMask_ = 0;
    std::array<std::vector<float>, kDiffusers> diffusers_[2];
    std::array<std::size_t, kDiffusers> diffuserLength_[2]{}, diffuserPos_[2]{};
    std::size_t pos_ = 0;
    std::size_t count_ = 0; // samples since the last reset: anything older is silence

    // The lines. Each setting has the value in use and the one it glides to.
    std::array<double, kLines> delay_{}, delayStep_{};
    std::array<double, kLines> gainLow_{}, gainMid_{}, gainHigh_{};
    std::array<double, kLines> gainLowTarget_{}, gainMidTarget_{}, gainHighTarget_{};
    std::array<double, kLines> bass_{}, treble_{};
    double bassCoeff_ = 0.0, trebleCoeff_ = 1.0;
    double scale_ = 1.0, scaleHalf_ = 1.0, scaleTarget_ = 1.0, sizeGlide_ = 1.0;
    double inGain_ = 0.0, lateGain_ = 0.0, inGainTarget_ = 0.0, lateGainTarget_ = 0.0, controlGlide_ = 1.0;

    // The wobble.
    std::array<std::array<double, 4>, kLines> modPoints_{};
    std::array<double, kLines> modPhase_{};
    double modSamples_ = 0.0, modSamplesTarget_ = 0.0, modStep_ = 0.0;
    std::uint32_t rng_ = 1;
    int controlCount_ = 0;

    // Input and output.
    double predelay_ = 0.0, predelayHalf_ = 0.0, predelayTarget_ = 0.0, predelayGlide_ = 1.0;
    double lowCutCoeff_ = 0.0, highCutCoeff_ = 1.0, colourCoeff_ = 1.0;
    std::array<double, 2> lowCut_[2]{}, highCut_[2]{}, colour_[2]{};
    bool earlyOn_ = false;
    std::array<double, kTaps> earlyLeft_{}, earlyRight_{}, earlyAge_{};
    double direct_ = 0.0, trim_ = 1.0;
    double dry_ = 1.0, wet_ = 0.0, width_ = 1.0;
    double dryTarget_ = 1.0, wetTarget_ = 0.0, widthTarget_ = 1.0, mixGlide_ = 1.0;
};

} // namespace synthmatch::dsp
