#include "synthmatch/match/Encoder.h"

#include "synthmatch/match/Features.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>

namespace synthmatch::match {

namespace {

constexpr std::size_t kMelBands = 40;
constexpr std::size_t kHarmonics = 20;
// Stretches of time, in seconds from the start of the note (while held) and
// from its release. Short at first, where sounds change fastest.
constexpr std::array<double, 7> kHoldEdges{0.0, 0.03, 0.08, 0.2, 0.5, 1.2, 3.0};
constexpr std::array<double, 5> kReleaseEdges{0.0, 0.05, 0.2, 0.6, 1.5};
constexpr std::size_t kSegments = (kHoldEdges.size() - 1) + (kReleaseEdges.size() - 1);
constexpr std::size_t kPerSegment = kMelBands + kHarmonics + 1;
constexpr std::array<double, 5> kWobbleHz{1.0, 2.0, 4.0, 8.0, 16.0};
constexpr std::size_t kWobbleFeatures = 2 * (1 + kWobbleHz.size());
constexpr std::size_t kFeatureCount = kSegments * kPerSegment + kWobbleFeatures + 3;

constexpr float kRangeDb = 80.0f;

// Maps a level in dB (0 = the loudest thing in the clip) to 0..1.
float unitLevel(float db) { return std::clamp(db, -kRangeDb, 0.0f) / kRangeDb + 1.0f; }

float powerDb(double power) { return static_cast<float>(10.0 * std::log10(power + 1.0e-12)); }

// How strongly a series wobbles overall, and at each test frequency.
// The slow drift (a straight-line fit) is removed first.
void wobble(std::vector<double> series, double frameRate, double fullScale, float* out)
{
    const std::size_t n = series.size();
    std::fill(out, out + 1 + kWobbleHz.size(), 0.0f);
    if (n < 12)
        return;

    double sumT = 0.0, sumX = 0.0, sumTT = 0.0, sumTX = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i);
        sumT += t;
        sumX += series[i];
        sumTT += t * t;
        sumTX += t * series[i];
    }
    const double count = static_cast<double>(n);
    const double slope = (count * sumTX - sumT * sumX) / (count * sumTT - sumT * sumT);
    const double offset = (sumX - slope * sumT) / count;

    double variance = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        series[i] -= offset + slope * static_cast<double>(i);
        variance += series[i] * series[i];
    }
    out[0] = static_cast<float>(std::min(1.0, std::sqrt(variance / count) / fullScale));

    const double seconds = count / frameRate;
    for (std::size_t m = 0; m < kWobbleHz.size(); ++m) {
        if (seconds * kWobbleHz[m] < 1.0)
            continue; // less than one cycle fits: can't tell
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double phase = 2.0 * std::numbers::pi * kWobbleHz[m] * static_cast<double>(i) / frameRate;
            re += series[i] * std::cos(phase);
            im += series[i] * std::sin(phase);
        }
        out[1 + m] = static_cast<float>(std::min(1.0, 2.0 * std::hypot(re, im) / count / fullScale));
    }
}

// Reading and writing the model file.
template <class T>
void put(std::vector<unsigned char>& out, const T& value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void putFloats(std::vector<unsigned char>& out, const std::vector<float>& values)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
    out.insert(out.end(), bytes, bytes + values.size() * sizeof(float));
}

struct Reader {
    std::span<const unsigned char> data;
    std::size_t pos = 0;
    bool ok = true;

    template <class T>
    T get()
    {
        T value{};
        if (pos + sizeof(T) > data.size()) {
            ok = false;
            return value;
        }
        std::memcpy(&value, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    std::vector<float> floats(std::size_t count)
    {
        std::vector<float> values;
        if (count > (data.size() - pos) / sizeof(float)) {
            ok = false;
            return values;
        }
        values.resize(count);
        std::memcpy(values.data(), data.data() + pos, count * sizeof(float));
        pos += count * sizeof(float);
        return values;
    }

    std::string text(std::size_t length)
    {
        if (length > data.size() - pos) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data.data() + pos), length);
        pos += length;
        return s;
    }
};

constexpr std::uint32_t kMagic = 0x4E454D53; // "SMEN"
constexpr std::uint32_t kVersion = 1;

static_assert(std::endian::native == std::endian::little, "model files are little-endian");

} // namespace

std::size_t encoderFeatureCount() { return kFeatureCount; }

std::vector<float> encoderFeatures(std::span<const float> input, double sampleRate, const NoteEvent& note, double endSec)
{
    std::vector<float> features(kFeatureCount, 0.0f);

    // Work at 22-44 kHz: the features only look up to 10 kHz.
    std::vector<float> reduced;
    std::span<const float> mono = input;
    double rate = sampleRate;
    while (rate >= 44100.0) {
        reduced = decimateBy2(mono);
        mono = reduced;
        rate /= 2.0;
    }

    const auto first = std::min(mono.size(), static_cast<std::size_t>(std::max(0.0, note.startSec) * rate));
    auto last = mono.size();
    if (endSec > note.startSec)
        last = std::min(last, static_cast<std::size_t>(endSec * rate));
    if (last <= first + 64)
        return features;
    mono = mono.subspan(first, last - first);

    SpectrogramConfig config;
    config.fftSize = rate <= 26000.0 ? 1024 : 2048;
    config.hop = config.fftSize / 4;
    config.numBands = kMelBands;
    config.minHz = 30.0;
    config.maxHz = 10000.0;
    const SpectrogramAnalyzer analyzer(rate, config);
    std::vector<float> power;
    const Spectrogram mel = analyzer.analyze(mono, &power);
    const std::size_t bins = analyzer.numBins();
    if (mel.numFrames == 0)
        return features;

    const double hopSec = static_cast<double>(config.hop) / rate;
    const double halfWindow = 0.5 * static_cast<double>(config.fftSize) / rate;
    const double binHz = rate / static_cast<double>(config.fftSize);
    const double f0 = 440.0 * std::exp2((note.note - 69) / 12.0);
    const double duration = std::max(0.02, note.durationSec);

    float framePeak = -200.0f;
    for (const float v : mel.frameDb)
        framePeak = std::max(framePeak, v);

    // Level of each harmonic in each frame: the strongest of the three bins around it.
    const auto harmonicPower = [&](std::size_t frame, std::size_t h) -> double {
        const double hz = static_cast<double>(h) * f0;
        if (hz > 0.47 * rate)
            return 0.0;
        const auto bin = static_cast<std::size_t>(std::lround(hz / binHz));
        double best = 0.0;
        for (std::size_t k = bin > 0 ? bin - 1 : 0; k <= std::min(bins - 1, bin + 1); ++k)
            best = std::max(best, static_cast<double>(power[frame * bins + k]));
        return best;
    };

    // Average each stretch of time.
    struct Stretch {
        double from, to; // seconds from the start of the note
        bool held;
    };
    std::array<Stretch, kSegments> stretches{};
    std::size_t s = 0;
    for (std::size_t i = 0; i + 1 < kHoldEdges.size(); ++i)
        stretches[s++] = {kHoldEdges[i], std::min(kHoldEdges[i + 1], duration), true};
    for (std::size_t i = 0; i + 1 < kReleaseEdges.size(); ++i)
        stretches[s++] = {duration + kReleaseEdges[i], duration + kReleaseEdges[i + 1], false};

    for (std::size_t seg = 0; seg < kSegments; ++seg) {
        float* out = features.data() + seg * kPerSegment;
        std::array<double, kPerSegment> sum{};
        std::size_t count = 0;
        for (std::size_t f = 0; f < mel.numFrames; ++f) {
            const double t = static_cast<double>(f) * hopSec + halfWindow;
            if (t < stretches[seg].from || t >= stretches[seg].to)
                continue;
            for (std::size_t b = 0; b < kMelBands; ++b)
                sum[b] += unitLevel(mel.at(f, b) - mel.peakDb);
            for (std::size_t h = 1; h <= kHarmonics; ++h)
                sum[kMelBands + h - 1] += unitLevel(powerDb(harmonicPower(f, h)) - mel.peakDb);
            sum[kMelBands + kHarmonics] += unitLevel(mel.frameDb[f] - framePeak);
            ++count;
        }
        if (count > 0) {
            for (std::size_t i = 0; i < kPerSegment; ++i)
                out[i] = static_cast<float>(sum[i] / static_cast<double>(count));
        } else if (stretches[seg].held && seg > 0) {
            // The note ended before this stretch: carry the last one forward.
            std::copy(out - kPerSegment, out, out);
        }
    }

    // Wobble of the level and of the brightness while the note is held, and
    // how much of the energy sits on the harmonics.
    std::vector<double> levels, brightness;
    double harmonic = 0.0, total = 0.0;
    for (std::size_t f = 0; f < mel.numFrames; ++f) {
        const double t = static_cast<double>(f) * hopSec + halfWindow;
        if (t < 0.08 || t >= duration)
            continue;
        levels.push_back(mel.frameDb[f]);

        double weighted = 0.0, bandTotal = 0.0;
        for (std::size_t b = 0; b < kMelBands; ++b) {
            const double p = std::pow(10.0, mel.at(f, b) / 10.0);
            weighted += p * analyzer.bandHz(b);
            bandTotal += p;
        }
        brightness.push_back(bandTotal > 0.0 ? std::log2(weighted / bandTotal) : 0.0);

        for (std::size_t k = 1; k < bins; ++k)
            total += power[f * bins + k];
        for (std::size_t h = 1; h * f0 < 0.47 * rate && h <= 200; ++h) {
            const auto bin = static_cast<std::size_t>(std::lround(static_cast<double>(h) * f0 / binHz));
            for (std::size_t k = bin > 0 ? bin - 1 : 0; k <= std::min(bins - 1, bin + 1); ++k)
                harmonic += power[f * bins + k];
        }
    }
    float* extra = features.data() + kSegments * kPerSegment;
    const double frameRate = 1.0 / hopSec;
    wobble(std::move(levels), frameRate, 6.0, extra);                             // full scale: 6 dB
    wobble(std::move(brightness), frameRate, 1.0, extra + 1 + kWobbleHz.size());  // full scale: 1 octave
    extra += kWobbleFeatures;
    extra[0] = total > 0.0 ? static_cast<float>(std::clamp(harmonic / total, 0.0, 1.0)) : 0.0f;
    extra[1] = static_cast<float>(note.note) / 127.0f;
    extra[2] = static_cast<float>(std::clamp(std::log2(duration / 0.05) / 6.0, 0.0, 1.0));
    return features;
}

std::size_t Mlp::parameterCount() const
{
    std::size_t count = 0;
    for (const auto& l : layers)
        count += l.weights.size() + l.biases.size();
    return count;
}

Mlp Mlp::create(std::span<const std::size_t> sizes, std::uint32_t seed)
{
    std::uint32_t state = seed * 747796405u + 2891336453u;
    if (state == 0)
        state = 1;
    const auto uniform = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (static_cast<double>(state) + 0.5) / 4294967296.0;
    };

    Mlp net;
    for (std::size_t i = 0; i + 1 < sizes.size(); ++i) {
        Layer layer;
        layer.inputs = sizes[i];
        layer.outputs = sizes[i + 1];
        layer.biases.assign(layer.outputs, 0.0f);
        layer.weights.resize(layer.inputs * layer.outputs);
        // "He" initialisation: random, scaled so signals neither die out nor
        // blow up as they pass through the ReLU layers.
        const double scale = std::sqrt(2.0 / static_cast<double>(layer.inputs));
        for (auto& w : layer.weights) {
            const double gaussian = std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * std::numbers::pi * uniform());
            w = static_cast<float>(scale * gaussian);
        }
        net.layers.push_back(std::move(layer));
    }
    return net;
}

std::vector<float> Mlp::forward(std::span<const float> input) const
{
    std::vector<float> current(input.begin(), input.end()), next;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const auto& layer = layers[l];
        next.assign(layer.outputs, 0.0f);
        if (current.size() != layer.inputs)
            return {};
        for (std::size_t o = 0; o < layer.outputs; ++o) {
            const float* w = layer.weights.data() + o * layer.inputs;
            float sum = layer.biases[o];
            for (std::size_t i = 0; i < layer.inputs; ++i)
                sum += w[i] * current[i];
            next[o] = l + 1 < layers.size() ? std::max(sum, 0.0f) : sum;
        }
        current.swap(next);
    }
    return current;
}

std::vector<double> decodeOutputs(const SearchSpace& space, std::span<const float> outputs)
{
    std::vector<double> x(space.dims.size(), 0.5);
    std::size_t at = 0;
    for (std::size_t d = 0; d < space.dims.size(); ++d) {
        const auto& dim = space.dims[d];
        if (dim.isChoice()) {
            const std::size_t n = dim.choices.size();
            if (at + n > outputs.size())
                break;
            std::size_t best = 0;
            for (std::size_t c = 1; c < n; ++c)
                if (outputs[at + c] > outputs[at + best])
                    best = c;
            x[d] = (static_cast<double>(best) + 0.5) / static_cast<double>(n);
            at += n;
        } else {
            if (at >= outputs.size())
                break;
            x[d] = 1.0 / (1.0 + std::exp(-static_cast<double>(outputs[at])));
            ++at;
        }
    }
    return x;
}

Encoder::Encoder(Mlp network, std::vector<float> inputMean, std::vector<float> inputScale)
    : network_(std::move(network)), inputMean_(std::move(inputMean)), inputScale_(std::move(inputScale))
{
}

std::vector<double> Encoder::predictFromFeatures(std::span<const float> features) const
{
    if (!valid() || features.size() != network_.inputSize())
        return {};
    std::vector<float> scaled(features.size());
    for (std::size_t i = 0; i < features.size(); ++i)
        scaled[i] = (features[i] - inputMean_[i]) * inputScale_[i];
    return decodeOutputs(SearchSpace::standard(), network_.forward(scaled));
}

std::vector<double> Encoder::predict(std::span<const float> mono, double sampleRate, const NoteEvent& note,
                                     double endSec) const
{
    return predictFromFeatures(encoderFeatures(mono, sampleRate, note, endSec));
}

bool Encoder::save(const std::filesystem::path& path, std::string* error) const
{
    const SearchSpace& space = SearchSpace::standard();
    std::vector<unsigned char> out;
    put(out, kMagic);
    put(out, kVersion);
    put(out, static_cast<std::uint32_t>(kFeatureCount));

    // Which parameters the outputs mean, so a model can't be used with a build
    // whose search space has changed.
    put(out, static_cast<std::uint32_t>(space.dims.size()));
    for (const auto& dim : space.dims) {
        const auto id = spec(dim.param).id;
        put(out, static_cast<std::uint32_t>(id.size()));
        out.insert(out.end(), id.begin(), id.end());
        put(out, static_cast<std::uint32_t>(dim.choices.size()));
    }

    put(out, static_cast<std::uint32_t>(network_.layers.size()));
    for (const auto& layer : network_.layers) {
        put(out, static_cast<std::uint32_t>(layer.inputs));
        put(out, static_cast<std::uint32_t>(layer.outputs));
        putFloats(out, layer.weights);
        putFloats(out, layer.biases);
    }
    putFloats(out, inputMean_);
    putFloats(out, inputScale_);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file)
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        if (error)
            *error = "cannot write " + path.string();
        return false;
    }
    return true;
}

std::optional<Encoder> Encoder::fromMemory(std::span<const unsigned char> data, std::string* error)
{
    const auto fail = [error](const char* message) -> std::optional<Encoder> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    constexpr const char* kRetrain = "This model was trained for a different version of SynthMatch. Train it again.";

    Reader in{data};
    if (in.get<std::uint32_t>() != kMagic || !in.ok)
        return fail("This is not a SynthMatch model file.");
    if (in.get<std::uint32_t>() != kVersion)
        return fail(kRetrain);
    if (in.get<std::uint32_t>() != kFeatureCount)
        return fail(kRetrain);

    const SearchSpace& space = SearchSpace::standard();
    if (in.get<std::uint32_t>() != space.dims.size())
        return fail(kRetrain);
    for (const auto& dim : space.dims) {
        const auto length = in.get<std::uint32_t>();
        const std::string id = in.text(length);
        const auto choices = in.get<std::uint32_t>();
        if (!in.ok || id != spec(dim.param).id || choices != dim.choices.size())
            return fail(kRetrain);
    }

    Mlp net;
    const auto numLayers = in.get<std::uint32_t>();
    if (!in.ok || numLayers == 0 || numLayers > 16)
        return fail("The model file is damaged.");
    for (std::uint32_t l = 0; l < numLayers; ++l) {
        Mlp::Layer layer;
        layer.inputs = in.get<std::uint32_t>();
        layer.outputs = in.get<std::uint32_t>();
        if (!in.ok || layer.inputs == 0 || layer.outputs == 0 || layer.inputs > 100000 || layer.outputs > 100000)
            return fail("The model file is damaged.");
        layer.weights = in.floats(layer.inputs * layer.outputs);
        layer.biases = in.floats(layer.outputs);
        if (!in.ok || (!net.layers.empty() && net.layers.back().outputs != layer.inputs))
            return fail("The model file is damaged.");
        net.layers.push_back(std::move(layer));
    }
    if (net.inputSize() != kFeatureCount || net.outputSize() != space.outputSize())
        return fail(kRetrain);

    auto mean = in.floats(kFeatureCount);
    auto scale = in.floats(kFeatureCount);
    if (!in.ok)
        return fail("The model file is damaged.");
    return Encoder(std::move(net), std::move(mean), std::move(scale));
}

std::optional<Encoder> Encoder::load(const std::filesystem::path& path, std::string* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error)
            *error = "cannot open " + path.string();
        return std::nullopt;
    }
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return fromMemory(data, error);
}

} // namespace synthmatch::match
