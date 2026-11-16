#include "synthmatch/match/Features.h"

#include "synthmatch/dsp/MatchEq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace synthmatch::match {

namespace {

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

std::vector<float> hannWindow(std::size_t size)
{
    std::vector<float> w(size);
    for (std::size_t i = 0; i < size; ++i)
        w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                                       static_cast<double>(size)));
    return w;
}

float toDb(double power) { return static_cast<float>(10.0 * std::log10(power + 1.0e-12)); }

} // namespace

SpectrogramAnalyzer::SpectrogramAnalyzer(double sampleRate, const SpectrogramConfig& config)
    : sampleRate_(sampleRate), config_(config), fft_(config.fftSize), window_(hannWindow(config.fftSize))
{
    const double maxHz = std::min(config_.maxHz, 0.49 * sampleRate_);
    const double melLow = hzToMel(config_.minHz), melHigh = hzToMel(maxHz);
    const double binHz = sampleRate_ / static_cast<double>(config_.fftSize);
    const std::size_t numBins = config_.fftSize / 2 + 1;

    // Triangular bands, evenly spaced on the mel scale (which is roughly how
    // the ear spaces pitch): narrow at the bottom, wide at the top.
    std::vector<double> edges(config_.numBands + 2);
    for (std::size_t i = 0; i < edges.size(); ++i)
        edges[i] = melToHz(melLow + (melHigh - melLow) * static_cast<double>(i) / static_cast<double>(edges.size() - 1));

    bands_.resize(config_.numBands);
    centres_.resize(config_.numBands);
    for (std::size_t b = 0; b < config_.numBands; ++b) {
        const double lo = edges[b], mid = edges[b + 1], hi = edges[b + 2];
        centres_[b] = mid;
        auto& band = bands_[b];
        band.firstBin = std::min(numBins - 1, static_cast<std::size_t>(std::ceil(lo / binHz)));
        for (std::size_t k = band.firstBin; k < numBins && static_cast<double>(k) * binHz < hi; ++k) {
            const double f = static_cast<double>(k) * binHz;
            const double w = f <= mid ? (f - lo) / (mid - lo) : (hi - f) / (hi - mid);
            band.weights.push_back(static_cast<float>(std::max(0.0, w)));
        }
        // A band narrower than one FFT bin still listens to its nearest bin.
        float total = 0.0f;
        for (const float w : band.weights)
            total += w;
        if (total < 0.25f) {
            band.firstBin = std::min(numBins - 1, static_cast<std::size_t>(std::lround(mid / binHz)));
            band.weights.assign(1, 1.0f);
        }
    }
}

Spectrogram SpectrogramAnalyzer::analyze(std::span<const float> mono, std::vector<float>* rawPower) const
{
    const std::size_t n = config_.fftSize, hop = config_.hop;
    Spectrogram out;
    out.numBands = config_.numBands;
    out.numFrames = mono.empty() ? 0 : (mono.size() + hop - 1) / hop;
    out.db.resize(out.numFrames * out.numBands);
    out.frameDb.resize(out.numFrames);

    double windowSum = 0.0;
    for (const float w : window_)
        windowSum += w;
    // Scaled so that a full-scale sine reads about 0 dB in its band.
    const double scale = 4.0 / (windowSum * windowSum);

    std::vector<std::complex<float>> buffer(n);
    std::vector<float> power(n / 2 + 1);
    if (rawPower)
        rawPower->resize(out.numFrames * power.size());

    for (std::size_t frame = 0; frame < out.numFrames; ++frame) {
        const std::size_t start = frame * hop;
        for (std::size_t i = 0; i < n; ++i) {
            const float x = start + i < mono.size() ? mono[start + i] : 0.0f;
            buffer[i] = {x * window_[i], 0.0f};
        }
        fft_.forward(buffer.data());

        double total = 0.0;
        for (std::size_t k = 0; k < power.size(); ++k) {
            power[k] = buffer[k].real() * buffer[k].real() + buffer[k].imag() * buffer[k].imag();
            total += power[k];
        }
        out.frameDb[frame] = toDb(total * scale);
        if (rawPower)
            for (std::size_t k = 0; k < power.size(); ++k)
                (*rawPower)[frame * power.size() + k] = static_cast<float>(power[k] * scale);

        for (std::size_t b = 0; b < out.numBands; ++b) {
            const auto& band = bands_[b];
            double sum = 0.0;
            for (std::size_t j = 0; j < band.weights.size(); ++j)
                sum += static_cast<double>(band.weights[j]) * power[band.firstBin + j];
            const float db = toDb(sum * scale);
            out.db[frame * out.numBands + b] = db;
            out.peakDb = std::max(out.peakDb, db);
        }
    }
    return out;
}

double spectralDistance(const Spectrogram& reference, const Spectrogram& candidate, float gainDb,
                        const DistanceConfig& config)
{
    if (reference.numFrames == 0 || reference.numBands != candidate.numBands)
        return 1.0e3;

    const float floor = reference.peakDb - config.rangeDb;
    const std::size_t frames = std::max(reference.numFrames, candidate.numFrames);
    const std::size_t bands = reference.numBands;

    // Cells are weighted by how loud they are, so the parts you hear most
    // count most, but quiet detail still counts for something.
    double sum = 0.0, weight = 0.0;
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t b = 0; b < bands; ++b) {
            const float r = f < reference.numFrames ? std::max(reference.at(f, b), floor) : floor;
            const float c = f < candidate.numFrames ? std::max(candidate.at(f, b) + gainDb, floor) : floor;
            const float loudest = std::max(r, c);
            if (loudest <= floor)
                continue;
            const double w = 0.25 + 0.75 * static_cast<double>(loudest - floor) / config.rangeDb;
            sum += w * std::abs(r - c);
            weight += w;
        }
    }
    const double spectral = weight > 0.0 ? sum / weight : 0.0;

    // The overall level over time (the "envelope").
    float framePeak = -200.0f;
    for (const float v : reference.frameDb)
        framePeak = std::max(framePeak, v);
    const float frameFloor = framePeak - config.rangeDb;
    double envSum = 0.0;
    std::size_t envCount = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        const float r = f < reference.numFrames ? std::max(reference.frameDb[f], frameFloor) : frameFloor;
        const float c = f < candidate.numFrames ? std::max(candidate.frameDb[f] + gainDb, frameFloor) : frameFloor;
        if (r <= frameFloor && c <= frameFloor)
            continue;
        envSum += std::abs(r - c);
        ++envCount;
    }
    const double envelope = envCount > 0 ? envSum / static_cast<double>(envCount) : 0.0;

    return spectral + config.envelopeWeight * envelope;
}

float bestGainDb(const Spectrogram& reference, const Spectrogram& candidate, float rangeDb)
{
    if (reference.numBands != candidate.numBands)
        return 0.0f;

    // Compare only the cells where the reference is clearly audible.
    const float strong = reference.peakDb - 0.5f * rangeDb;
    const std::size_t frames = std::min(reference.numFrames, candidate.numFrames);
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t b = 0; b < reference.numBands; ++b) {
            const float r = reference.at(f, b), c = candidate.at(f, b);
            if (r > strong && c > -110.0f) {
                sum += r - c;
                ++count;
            }
        }
    }
    return count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
}

namespace {

// Mean power spectrum (Welch's method: Hann-windowed frames, averaged).
std::vector<double> meanPowerSpectrum(std::span<const float> mono, std::size_t n, std::size_t hop)
{
    const Fft fft(n);
    const auto window = hannWindow(n);

    std::vector<double> mean(n / 2 + 1, 0.0);
    std::vector<std::complex<float>> buffer(n);
    const std::size_t frames = mono.empty() ? 0 : (mono.size() + hop - 1) / hop;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t start = frame * hop;
        for (std::size_t i = 0; i < n; ++i) {
            const float x = start + i < mono.size() ? mono[start + i] : 0.0f;
            buffer[i] = {x * window[i], 0.0f};
        }
        fft.forward(buffer.data());
        for (std::size_t k = 0; k < mean.size(); ++k)
            mean[k] += static_cast<double>(std::norm(buffer[k]));
    }
    if (frames > 0)
        for (auto& v : mean)
            v /= static_cast<double>(frames);
    return mean;
}

// Power in dB between lo and hi Hz, counting partly covered bins in proportion.
float bandPowerDb(const std::vector<double>& mean, double binHz, double lo, double hi)
{
    double power = 0.0;
    const auto first = static_cast<std::size_t>(std::floor(lo / binHz));
    const auto last = std::min(mean.size() - 1, static_cast<std::size_t>(std::floor(hi / binHz)));
    for (std::size_t k = first; k <= last; ++k) {
        const double binLo = (static_cast<double>(k) - 0.5) * binHz, binHi = binLo + binHz;
        const double overlap = std::max(0.0, std::min(hi, binHi) - std::max(lo, binLo));
        power += mean[k] * overlap / binHz;
    }
    return static_cast<float>(10.0 * std::log10(power + 1.0e-20));
}

} // namespace

MatchEqGains thirdOctaveSpectrum(std::span<const float> mono, double sampleRate)
{
    constexpr std::size_t n = 8192;
    const auto mean = meanPowerSpectrum(mono, n, 2048);
    const double binHz = sampleRate / static_cast<double>(n);
    MatchEqGains out{};
    for (int band = 0; band < kMatchEqBands; ++band) {
        const double centre = dsp::matchEqBandHz(band);
        out[static_cast<std::size_t>(band)] =
            centre >= 0.45 * sampleRate ? -200.0f
                                        : bandPowerDb(mean, binHz, centre * std::exp2(-1.0 / 6.0), centre * std::exp2(1.0 / 6.0));
    }
    return out;
}

double fineSpectrumHz(int band) { return 20.0 * std::exp2(static_cast<double>(band) / 12.0); }

std::vector<float> fineSpectrum(std::span<const float> mono, double sampleRate)
{
    // A long frame (about 3 Hz per bin at 48 kHz), so even the lowest bands,
    // a few hertz wide, are resolved.
    const std::size_t n = sampleRate > 60000.0 ? 32768 : 16384;
    const auto mean = meanPowerSpectrum(mono, n, n / 4);
    const double binHz = sampleRate / static_cast<double>(n);
    std::vector<float> out(kFineSpectrumBands, -200.0f);
    for (int band = 0; band < kFineSpectrumBands; ++band) {
        const double centre = fineSpectrumHz(band);
        if (centre < 0.45 * sampleRate)
            out[static_cast<std::size_t>(band)] =
                bandPowerDb(mean, binHz, centre * std::exp2(-1.0 / 24.0), centre * std::exp2(1.0 / 24.0));
    }
    return out;
}

MatchEqGains matchEqCurve(const MatchEqGains& referenceDb, const MatchEqGains& candidateDb, double sampleRate,
                          double maxDb)
{
    float refPeak = -200.0f, candPeak = -200.0f;
    for (std::size_t b = 0; b < referenceDb.size(); ++b) {
        refPeak = std::max(refPeak, referenceDb[b]);
        candPeak = std::max(candPeak, candidateDb[b]);
    }

    // A band is corrected only where both sounds have something there: EQ can't
    // create what the synth doesn't produce, and shouldn't chase the noise floor.
    MatchEqGains diff{};
    std::array<bool, kMatchEqBands> valid{};
    std::vector<float> validDiffs;
    for (int band = 0; band < kMatchEqBands; ++band) {
        const auto b = static_cast<std::size_t>(band);
        valid[b] = dsp::matchEqBandHz(band) < 0.45 * sampleRate && referenceDb[b] > refPeak - 55.0f &&
                   candidateDb[b] > candPeak - 70.0f;
        if (valid[b]) {
            diff[b] = referenceDb[b] - candidateDb[b];
            validDiffs.push_back(diff[b]);
        }
    }
    if (validDiffs.empty())
        return {};

    // The overall level is the gain parameter's job, so remove the typical
    // difference and keep only the shape.
    std::nth_element(validDiffs.begin(), validDiffs.begin() + static_cast<std::ptrdiff_t>(validDiffs.size() / 2),
                     validDiffs.end());
    const float median = validDiffs[validDiffs.size() / 2];

    MatchEqGains shaped{};
    for (std::size_t b = 0; b < shaped.size(); ++b)
        shaped[b] = valid[b] ? diff[b] - median : 0.0f;

    // Light smoothing across neighbouring bands, then limit.
    MatchEqGains out{};
    for (std::size_t b = 0; b < out.size(); ++b) {
        const float prev = b > 0 ? shaped[b - 1] : shaped[b];
        const float next = b + 1 < out.size() ? shaped[b + 1] : shaped[b];
        const float smooth = 0.25f * prev + 0.5f * shaped[b] + 0.25f * next;
        out[b] = valid[b] ? std::clamp(smooth, static_cast<float>(-maxDb), static_cast<float>(maxDb)) : 0.0f;
    }
    return out;
}

MatchEqGains fitMatchEq(const MatchEqGains& targetDb, double sampleRate)
{
    MatchEqGains gains = targetDb;
    for (int band = 0; band < kMatchEqBands; ++band)
        if (dsp::matchEqBandHz(band) >= 0.45 * sampleRate)
            gains[static_cast<std::size_t>(band)] = 0.0f;

    // Neighbouring bells overlap, so setting each band to its target overshoots.
    // Repeatedly nudge every band by its remaining error.
    for (int iteration = 0; iteration < 40; ++iteration) {
        const auto coeffs = dsp::matchEqCoeffs(sampleRate, gains);
        for (int band = 0; band < kMatchEqBands; ++band) {
            const auto b = static_cast<std::size_t>(band);
            const double hz = dsp::matchEqBandHz(band);
            if (hz >= 0.45 * sampleRate)
                continue;
            const double error = targetDb[b] - dsp::matchEqGainDbAt(coeffs, hz, sampleRate);
            gains[b] = static_cast<float>(
                std::clamp(gains[b] + 0.5 * error, -dsp::kMatchEqMaxDb, dsp::kMatchEqMaxDb));
        }
    }
    return gains;
}

std::vector<float> toMono(std::span<const float> left, std::span<const float> right)
{
    std::vector<float> mono(std::min(left.size(), right.size()));
    for (std::size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5f * (left[i] + right[i]);
    return mono;
}

std::vector<float> decimateBy2(std::span<const float> x)
{
    // Windowed-sinc low-pass just under a quarter of the sample rate.
    constexpr int taps = 63, centre = taps / 2;
    constexpr double cutoff = 0.23; // cycles per sample
    std::array<double, taps> h{};
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double t = i - centre;
        const double sinc = t == 0.0 ? 2.0 * cutoff : std::sin(2.0 * std::numbers::pi * cutoff * t) / (std::numbers::pi * t);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (taps - 1)) +
                         0.08 * std::cos(4.0 * std::numbers::pi * i / (taps - 1)); // Blackman
        h[static_cast<std::size_t>(i)] = sinc * w;
        sum += h[static_cast<std::size_t>(i)];
    }
    for (auto& v : h)
        v /= sum;

    std::vector<float> out(x.size() / 2);
    const auto size = static_cast<std::ptrdiff_t>(x.size());
    for (std::size_t n = 0; n < out.size(); ++n) {
        double acc = 0.0;
        const auto at = static_cast<std::ptrdiff_t>(2 * n);
        for (int i = 0; i < taps; ++i) {
            const std::ptrdiff_t j = at + centre - i;
            if (j >= 0 && j < size)
                acc += h[static_cast<std::size_t>(i)] * x[static_cast<std::size_t>(j)];
        }
        out[n] = static_cast<float>(acc);
    }
    return out;
}

} // namespace synthmatch::match
