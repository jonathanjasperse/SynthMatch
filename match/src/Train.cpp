#include "synthmatch/match/Train.h"

#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/Oscillator.h"
#include "synthmatch/match/Features.h"
#include "synthmatch/match/Matcher.h"
#include "synthmatch/match/Modulation.h"
#include "synthmatch/match/Parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <numbers>
#include <numeric>
#include <vector>

namespace synthmatch::match {

namespace {

class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed * 2654435761u + 0x7F4A7C15u)
    {
        if (state_ == 0)
            state_ = 1;
        for (int i = 0; i < 4; ++i)
            next();
    }

    std::uint32_t next()
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    double uniform() { return static_cast<double>(next()) / 4294967296.0; }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    double logRange(double lo, double hi) { return lo * std::pow(hi / lo, uniform()); }
    bool chance(double probability) { return uniform() < probability; }

    // Picks an index with the given relative weights.
    std::size_t pick(std::initializer_list<double> weights)
    {
        double total = 0.0;
        for (const double w : weights)
            total += w;
        double r = uniform() * total;
        std::size_t index = 0;
        for (const double w : weights) {
            if (r < w)
                return index;
            r -= w;
            ++index;
        }
        return weights.size() - 1;
    }

private:
    std::uint32_t state_;
};

constexpr double kRegressionWeight = 6.0; // balances the two kinds of output in the loss

struct Dataset {
    std::size_t count = 0, inputs = 0, outputs = 0;
    std::vector<float> features, targets, mask;

    const float* x(std::size_t i) const { return features.data() + i * inputs; }
    const float* t(std::size_t i) const { return targets.data() + i * outputs; }
    const float* m(std::size_t i) const { return mask.data() + i * outputs; }
};

// The training target for a preset: for a continuous dimension, its position
// in 0..1; for a choice dimension, a 1 in the slot of the right choice. The
// mask is 0 for dimensions that can't be heard in this preset.
void makeTargets(const SearchSpace& space, const ParamValues& params, float* targets, float* mask)
{
    const auto audible = audibleDims(space, params);
    std::size_t at = 0;
    for (std::size_t d = 0; d < space.dims.size(); ++d) {
        const auto& dim = space.dims[d];
        const float value = params[static_cast<std::size_t>(dim.param)];
        const float heard = audible[d] ? 1.0f : 0.0f;
        if (dim.isChoice()) {
            const std::size_t right = dim.choiceIndex(value);
            for (std::size_t c = 0; c < dim.choices.size(); ++c) {
                targets[at + c] = c == right ? 1.0f : 0.0f;
                mask[at + c] = heard;
            }
            at += dim.choices.size();
        } else {
            targets[at] = static_cast<float>(dim.fromValue(value));
            mask[at] = heard;
            ++at;
        }
    }
}

// Working memory for one thread: layer outputs, error signals and gradients.
struct Workspace {
    std::vector<std::vector<float>> activations; // [0] = input copy, then one per layer
    std::vector<std::vector<float>> deltas;      // one per layer
    std::vector<std::vector<float>> gradWeights, gradBiases;
    double loss = 0.0;

    explicit Workspace(const Mlp& net)
    {
        activations.resize(net.layers.size() + 1);
        activations[0].resize(net.inputSize());
        for (std::size_t l = 0; l < net.layers.size(); ++l) {
            activations[l + 1].resize(net.layers[l].outputs);
            deltas.emplace_back(net.layers[l].outputs);
            gradWeights.emplace_back(net.layers[l].weights.size());
            gradBiases.emplace_back(net.layers[l].biases.size());
        }
    }

    void clear()
    {
        loss = 0.0;
        for (auto& g : gradWeights)
            std::fill(g.begin(), g.end(), 0.0f);
        for (auto& g : gradBiases)
            std::fill(g.begin(), g.end(), 0.0f);
    }
};

void forward(const Mlp& net, const float* input, Workspace& ws)
{
    std::copy(input, input + net.inputSize(), ws.activations[0].begin());
    for (std::size_t l = 0; l < net.layers.size(); ++l) {
        const auto& layer = net.layers[l];
        const float* in = ws.activations[l].data();
        float* out = ws.activations[l + 1].data();
        const bool last = l + 1 == net.layers.size();
        for (std::size_t o = 0; o < layer.outputs; ++o) {
            const float* w = layer.weights.data() + o * layer.inputs;
            float sum = 0.0f;
            for (std::size_t i = 0; i < layer.inputs; ++i)
                sum += w[i] * in[i];
            sum += layer.biases[o];
            out[o] = last ? sum : std::max(sum, 0.0f);
        }
    }
}

// The loss for one example and, in `delta`, its slope with respect to each raw output.
double outputLoss(const SearchSpace& space, const float* outputs, const float* targets, const float* mask, float* delta)
{
    double loss = 0.0;
    std::size_t at = 0;
    for (const auto& dim : space.dims) {
        if (dim.isChoice()) {
            const std::size_t n = dim.choices.size();
            if (mask[at] > 0.0f) {
                // Softmax and cross-entropy: how surprised the network is by the right answer.
                float top = outputs[at];
                for (std::size_t c = 1; c < n; ++c)
                    top = std::max(top, outputs[at + c]);
                double sum = 0.0;
                for (std::size_t c = 0; c < n; ++c)
                    sum += std::exp(static_cast<double>(outputs[at + c] - top));
                for (std::size_t c = 0; c < n; ++c) {
                    const double p = std::exp(static_cast<double>(outputs[at + c] - top)) / sum;
                    delta[at + c] = static_cast<float>(p - targets[at + c]);
                    if (targets[at + c] > 0.5f)
                        loss -= std::log(std::max(p, 1.0e-12));
                }
            } else {
                std::fill(delta + at, delta + at + n, 0.0f);
            }
            at += n;
        } else {
            if (mask[at] > 0.0f) {
                // Squared error on the sigmoid of the output.
                const double s = 1.0 / (1.0 + std::exp(-static_cast<double>(outputs[at])));
                const double error = s - targets[at];
                loss += kRegressionWeight * error * error;
                delta[at] = static_cast<float>(kRegressionWeight * 2.0 * error * s * (1.0 - s));
            } else {
                delta[at] = 0.0f;
            }
            ++at;
        }
    }
    return loss;
}

// Adds one example's gradients to the workspace (backpropagation).
void accumulate(const Mlp& net, const SearchSpace& space, const float* input, const float* targets, const float* mask,
                Workspace& ws)
{
    forward(net, input, ws);
    const std::size_t numLayers = net.layers.size();
    ws.loss += outputLoss(space, ws.activations[numLayers].data(), targets, mask, ws.deltas[numLayers - 1].data());

    for (std::size_t l = numLayers; l-- > 0;) {
        const auto& layer = net.layers[l];
        const float* delta = ws.deltas[l].data();
        const float* in = ws.activations[l].data();
        float* gw = ws.gradWeights[l].data();
        float* gb = ws.gradBiases[l].data();

        for (std::size_t o = 0; o < layer.outputs; ++o) {
            const float d = delta[o];
            if (d == 0.0f)
                continue;
            float* row = gw + o * layer.inputs;
            for (std::size_t i = 0; i < layer.inputs; ++i)
                row[i] += d * in[i];
            gb[o] += d;
        }

        if (l == 0)
            break;
        // Pass the error back through this layer, and through the ReLU before it.
        float* previous = ws.deltas[l - 1].data();
        std::fill(previous, previous + layer.inputs, 0.0f);
        for (std::size_t o = 0; o < layer.outputs; ++o) {
            const float d = delta[o];
            if (d == 0.0f)
                continue;
            const float* row = layer.weights.data() + o * layer.inputs;
            for (std::size_t i = 0; i < layer.inputs; ++i)
                previous[i] += d * row[i];
        }
        for (std::size_t i = 0; i < layer.inputs; ++i)
            if (in[i] <= 0.0f)
                previous[i] = 0.0f;
    }
}

// Adam: gradient descent with a running average of the gradient and of its
// size, so every weight gets a step that suits it.
class Adam {
public:
    explicit Adam(const Mlp& net)
    {
        for (const auto& layer : net.layers) {
            m_.emplace_back(layer.weights.size() + layer.biases.size(), 0.0f);
            v_.emplace_back(layer.weights.size() + layer.biases.size(), 0.0f);
        }
    }

    void step(Mlp& net, const Workspace& grads, double learningRate, double scale)
    {
        ++t_;
        const double correction = std::sqrt(1.0 - std::pow(kBeta2, t_)) / (1.0 - std::pow(kBeta1, t_));
        const auto rate = static_cast<float>(learningRate * correction);
        const auto s = static_cast<float>(scale);

        for (std::size_t l = 0; l < net.layers.size(); ++l) {
            auto& layer = net.layers[l];
            update(layer.weights.data(), grads.gradWeights[l].data(), m_[l].data(), v_[l].data(),
                   layer.weights.size(), rate, s);
            update(layer.biases.data(), grads.gradBiases[l].data(), m_[l].data() + layer.weights.size(),
                   v_[l].data() + layer.weights.size(), layer.biases.size(), rate, s);
        }
    }

private:
    static constexpr double kBeta1 = 0.9, kBeta2 = 0.999;

    static void update(float* w, const float* g, float* m, float* v, std::size_t n, float rate, float scale)
    {
        constexpr float b1 = 0.9f, b2 = 0.999f, eps = 1.0e-8f;
        for (std::size_t i = 0; i < n; ++i) {
            const float grad = g[i] * scale;
            m[i] = b1 * m[i] + (1.0f - b1) * grad;
            v[i] = b2 * v[i] + (1.0f - b2) * grad * grad;
            w[i] -= rate * m[i] / (std::sqrt(v[i]) + eps);
        }
    }

    std::vector<std::vector<float>> m_, v_;
    double t_ = 0.0;
};

double meanLoss(const Mlp& net, const SearchSpace& space, const Dataset& data, std::size_t from, std::size_t to,
                unsigned threads)
{
    if (to <= from)
        return 0.0;
    const std::size_t chunk = 256;
    const std::size_t chunks = (to - from + chunk - 1) / chunk;
    std::vector<double> chunkLoss(chunks, 0.0);
    parallelFor(chunks, threads, [&](std::size_t c) {
        Workspace ws(net);
        std::vector<float> delta(net.outputSize());
        double loss = 0.0;
        for (std::size_t i = from + c * chunk; i < std::min(to, from + (c + 1) * chunk); ++i) {
            forward(net, data.x(i), ws);
            loss += outputLoss(space, ws.activations.back().data(), data.t(i), data.m(i), delta.data());
        }
        chunkLoss[c] = loss;
    });
    return std::accumulate(chunkLoss.begin(), chunkLoss.end(), 0.0) / static_cast<double>(to - from);
}

// Makes a clean render sound a little more like a recording: now and then a
// gentle EQ tilt, some saturation, a small room or a low noise floor. The
// preset (the answer) stays the same, so the network learns to hear past them.
void augment(StereoBuffer& audio, std::uint32_t seed)
{
    Rng rng(seed * 2654435761u + 977u);
    const double sr = audio.sampleRate;
    float peak = 0.0f;
    for (const auto* channel : {&audio.left, &audio.right})
        for (const float v : *channel)
            peak = std::max(peak, std::abs(v));
    if (peak <= 1.0e-6f)
        return;

    // Tilt: a few dB more or less above about 1 kHz.
    if (rng.chance(0.5)) {
        const double tilt = rng.range(-0.3, 0.4);
        const double a = std::exp(-2.0 * std::numbers::pi * 1000.0 / sr);
        for (auto* channel : {&audio.left, &audio.right}) {
            double low = 0.0;
            for (auto& v : *channel) {
                low = (1.0 - a) * v + a * low;
                v = static_cast<float>(low + (1.0 + tilt) * (v - low));
            }
        }
    }

    // Saturation: soft clipping, as from a driven preamp or tape.
    if (rng.chance(0.3)) {
        const double drive = rng.range(1.2, 3.5), norm = std::tanh(drive);
        for (auto* channel : {&audio.left, &audio.right})
            for (auto& v : *channel)
                v = static_cast<float>(peak * std::tanh(drive * v / peak) / norm);
    }

    // A small room: four feedback echoes (a Schroeder comb bank), mixed in quietly.
    if (rng.chance(0.3)) {
        const double size = rng.range(0.5, 1.0), wet = rng.range(0.05, 0.2), decaySec = rng.range(0.2, 0.6);
        const std::size_t n = audio.numFrames();
        std::vector<float> room(n, 0.0f);
        for (const double ms : {29.7, 37.1, 41.1, 43.7}) {
            const auto delay = std::max<std::size_t>(1, static_cast<std::size_t>(ms * size * 0.001 * sr));
            const double feedback = std::pow(10.0, -3.0 * (static_cast<double>(delay) / sr) / decaySec); // -60 dB after decaySec
            std::vector<float> line(delay, 0.0f);
            std::size_t pos = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const float out = line[pos];
                line[pos] = static_cast<float>(0.5 * (audio.left[i] + audio.right[i]) + feedback * out);
                pos = pos + 1 == delay ? 0 : pos + 1;
                room[i] += 0.25f * out;
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            audio.left[i] += static_cast<float>(wet * room[i]);
            audio.right[i] += static_cast<float>(wet * room[i]);
        }
    }

    // A noise floor 40 to 70 dB below the peak.
    if (rng.chance(0.5)) {
        const double level = peak * std::pow(10.0, rng.range(-70.0, -40.0) / 20.0);
        for (auto* channel : {&audio.left, &audio.right})
            for (auto& v : *channel)
                v += static_cast<float>(level * (2.0 * rng.uniform() - 1.0));
    }
}

} // namespace

RandomSound randomSound(const SearchSpace& space, std::uint32_t seed)
{
    Rng rng(seed);
    RandomSound sound;
    ParamValues& p = sound.params;
    p = space.base;
    const auto set = [&](int index, double value) {
        p[static_cast<std::size_t>(index)] = clampToRange(spec(index), static_cast<float>(value));
    };

    // Oscillators. The first is always on; the others often aren't.
    const auto randomWave = [&] { return static_cast<double>(rng.pick({0.2, 0.15, 0.35, 0.3})); }; // sine, tri, saw, square
    set(param::Osc1Wave, randomWave());
    set(param::Osc1Level, rng.range(0.5, 1.0));
    set(param::Osc1PulseWidth, rng.range(0.1, 0.9));
    const double onChance[] = {1.0, 0.5, 0.25};
    for (int k = 1; k < param::kNumOscs; ++k) {
        set(param::osc(k, param::OscWave), randomWave());
        set(param::osc(k, param::OscPulseWidth), rng.range(0.1, 0.9));
        const bool on = rng.chance(onChance[k]);
        set(param::osc(k, param::OscLevel), on ? rng.range(0.2, 1.0) : 0.0);
        const double intervals[] = {-24.0, -12.0, 0.0, 7.0, 12.0, 19.0, 24.0};
        const double coarse = intervals[rng.pick({0.04, 0.2, 0.4, 0.08, 0.2, 0.04, 0.04})];
        set(param::osc(k, param::OscCoarse), coarse);
        // At the same pitch as oscillator 1 a detune is typical; otherwise it's small.
        const double detune = coarse == 0.0 ? kMaxOscDetuneCents : std::min(8.0, double{kMaxOscDetuneCents});
        set(param::osc(k, param::OscFine), rng.range(-detune, detune));
    }
    set(param::NoiseLevel, rng.chance(0.2) ? rng.logRange(0.02, 0.6) : 0.0);

    // Filter.
    const auto type = static_cast<dsp::FilterType>(rng.pick({0.7, 0.15, 0.15}));
    set(param::FilterType, static_cast<double>(type));
    switch (type) {
    case dsp::FilterType::BandPass:
        set(param::FilterCutoffHz, rng.logRange(200.0, 8000.0));
        break;
    case dsp::FilterType::HighPass:
        set(param::FilterCutoffHz, rng.logRange(30.0, 3000.0));
        break;
    default:
        set(param::FilterCutoffHz, rng.logRange(100.0, 20000.0));
        break;
    }
    const double resonance = rng.uniform();
    set(param::FilterResonance, 0.9 * resonance * resonance); // mostly low
    set(param::FilterKeyTrack, rng.chance(0.5) ? 0.0 : rng.uniform());
    set(param::FilterEnvAmount, rng.chance(0.4) ? 0.0 : rng.range(-3.0, 6.0));
    set(param::FilterAttackMs, rng.logRange(0.1, 500.0));
    set(param::FilterDecayMs, rng.logRange(20.0, 3000.0));
    set(param::FilterSustain, rng.uniform());
    set(param::FilterReleaseMs, rng.logRange(20.0, 2000.0));

    // Amp envelope: sustained sounds and plucks are both common.
    set(param::AmpAttackMs, rng.logRange(0.1, 800.0));
    set(param::AmpDecayMs, rng.logRange(20.0, 4000.0));
    const std::size_t sustainKind = rng.pick({0.35, 0.15, 0.5});
    set(param::AmpSustain, sustainKind == 0 ? 1.0 : (sustainKind == 1 ? 0.0 : rng.uniform()));
    set(param::AmpReleaseMs, rng.logRange(5.0, 3000.0));

    // Unison.
    const double voices[] = {1.0, 2.0, 3.0, 5.0, 7.0};
    set(param::UnisonVoices, voices[rng.pick({0.6, 0.1, 0.12, 0.1, 0.08})]);
    set(param::UnisonDetune, rng.range(3.0, double{kMaxUnisonDetuneCents}));

    // LFO: usually off; vibrato and filter movement are the common uses.
    // (Each target takes the same number of draws, so the settings after
    // this don't depend on it.)
    const auto target = static_cast<dsp::LfoTarget>(rng.pick({0.6, 0.18, 0.12, 0.06, 0.04}));
    const bool vibratoOrTremolo = target == dsp::LfoTarget::Pitch || target == dsp::LfoTarget::Amp;
    set(param::Lfo1Target, static_cast<double>(target));
    // Vibrato and tremolo are nearly always smooth; other uses take any shape.
    const std::size_t lfoWave = vibratoOrTremolo ? rng.pick({0.7, 0.25, 0.05 / 3.0, 0.05 / 3.0, 0.05 / 3.0})
                                                 : rng.pick({0.5, 0.2, 0.1, 0.1, 0.1});
    set(param::Lfo1Wave, static_cast<double>(lfoWave));
    const double rateDraw = rng.uniform();
    const auto rateBetween = [&](double lo, double hi) { return lo * std::pow(hi / lo, rateDraw); };
    switch (target) {
    case dsp::LfoTarget::Pitch:
        // Vibrato, sized in cents: from barely there to wide.
        set(param::Lfo1Depth, vibratoDepth(rng.logRange(5.0, 80.0)));
        set(param::Lfo1RateHz, rateBetween(3.5, 8.5));
        break;
    case dsp::LfoTarget::Amp:
        set(param::Lfo1RateHz, rateBetween(2.0, 12.0));
        set(param::Lfo1Depth, rng.range(0.1, 0.7));
        break;
    case dsp::LfoTarget::Cutoff:
        set(param::Lfo1RateHz, rateBetween(0.5, 15.0));
        set(param::Lfo1Depth, rng.range(0.05, 0.6));
        break;
    default:
        set(param::Lfo1RateHz, rateBetween(0.5, 15.0));
        set(param::Lfo1Depth, rng.range(0.1, 1.0));
        break;
    }

    // Effects.
    set(param::DelayOn, rng.chance(0.2) ? 1.0 : 0.0);
    set(param::DelayTimeMs, rng.logRange(30.0, 800.0));
    set(param::DelayFeedback, rng.range(0.0, 0.8));
    set(param::DelayMix, rng.range(0.05, 0.5));
    set(param::DelayHighCutHz, rng.logRange(1000.0, 20000.0));

    set(param::ReverbOn, rng.chance(0.4) ? 1.0 : 0.0);
    set(param::ReverbMix, rng.range(0.05, 0.6));
    set(param::ReverbSize, rng.uniform());
    set(param::ReverbDecaySec, rng.logRange(0.3, 8.0));
    set(param::ReverbDamping, rng.uniform());
    // Room, chamber, hall, plate, vintage. The wobble, pre-delay and high cut
    // aren't searched, but vary so the encoder doesn't depend on them.
    set(param::ReverbMode, static_cast<double>(rng.pick({0.2, 0.15, 0.3, 0.25, 0.1})));
    set(param::ReverbModDepth, rng.range(0.0, 0.7));
    set(param::ReverbPredelayMs, rng.range(0.0, 60.0));
    set(param::ReverbHighCutHz, rng.chance(0.5) ? 20000.0 : rng.logRange(5000.0, 20000.0));

    // The note: anywhere from a low bass to a high lead, held for a short or long time.
    sound.note.startSec = 0.0;
    sound.note.note = 28 + static_cast<int>(rng.uniform() * 61.0); // E1 to E6
    // Long enough to hear a few cycles of vibrato or tremolo.
    sound.note.durationSec = vibratoOrTremolo ? rng.logRange(0.7, 2.5) : rng.logRange(0.15, 1.6);
    sound.note.velocity = 1.0f;
    sound.seconds = sound.note.durationSec + 1.5;
    return sound;
}

TrainResult trainEncoder(const TrainOptions& options)
{
    TrainResult result;
    const SearchSpace& space = SearchSpace::standard();
    const unsigned threads = threadCount(options.threads);
    const auto log = [&](const std::string& line) {
        if (options.log)
            options.log(line);
    };
    const auto cancelled = [&] { return options.cancel && options.cancel->load(); };
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };

    // 1. Make the training data.
    Dataset data;
    // Graded real recordings first, then the synthetic notes (the validation
    // set is taken from the end, so it stays purely synthetic and comparable).
    const std::size_t repeat = static_cast<std::size_t>(std::max(1, options.extraRepeat));
    std::size_t extraTotal = 0;
    for (const auto& e : options.extra)
        if (e.features.size() == encoderFeatureCount())
            extraTotal += repeat;
    data.count = extraTotal + options.samples;
    data.inputs = encoderFeatureCount();
    data.outputs = space.outputSize();
    data.features.resize(data.count * data.inputs);
    data.targets.resize(data.count * data.outputs);
    data.mask.resize(data.count * data.outputs);

    if (extraTotal > 0) {
        std::size_t at = 0;
        for (const auto& e : options.extra) {
            if (e.features.size() != encoderFeatureCount())
                continue;
            for (std::size_t r = 0; r < repeat; ++r, ++at) {
                std::copy(e.features.begin(), e.features.end(), data.features.begin() + static_cast<std::ptrdiff_t>(at * data.inputs));
                makeTargets(space, e.params, data.targets.data() + at * data.outputs, data.mask.data() + at * data.outputs);
            }
        }
        log("Added " + std::to_string(extraTotal / repeat) + " graded recordings (each repeated " + std::to_string(repeat) +
            " times).");
    }

    // The last `validation` notes are held back to measure progress.
    const auto validation = static_cast<std::size_t>(static_cast<double>(options.samples) * options.validationFraction);

    log("Rendering " + std::to_string(options.samples) + " training notes on " + std::to_string(threads) + " threads" +
        (options.detectNotes ? ", listening for the notes as matching does" : "") +
        (options.augment ? ", with recording-like extras" : "") + "...");
    std::atomic<std::size_t> done{0};
    parallelFor(options.samples, threads, [&](std::size_t k) {
        if (cancelled())
            return;
        const std::size_t i = extraTotal + k;
        const std::uint32_t seed = options.seed * 1000003u + static_cast<std::uint32_t>(k);
        const RandomSound sound = randomSound(space, seed);
        RenderSettings settings;
        settings.sampleRate = options.sampleRate;
        settings.tailSec = sound.seconds - sound.note.durationSec;
        const NoteEvent notes[] = {sound.note};
        auto audio = render(sound.params, notes, settings);

        // Held-back notes stay clean and use the true note, so the validation
        // loss stays comparable between runs.
        const bool heldBack = k >= options.samples - validation;
        if (options.augment && !heldBack)
            augment(audio, seed);
        std::vector<float> features;
        if (options.detectNotes && !heldBack)
            features = encoderInputFor(audio);
        if (features.size() != encoderFeatureCount()) // no note found: use the true one
            features = encoderFeatures(toMono(audio.left, audio.right), options.sampleRate, sound.note);
        std::copy(features.begin(), features.end(), data.features.begin() + static_cast<std::ptrdiff_t>(i * data.inputs));
        makeTargets(space, sound.params, data.targets.data() + i * data.outputs, data.mask.data() + i * data.outputs);
        ++done;
    });
    if (cancelled())
        return result;
    {
        char line[96];
        std::snprintf(line, sizeof line, "Rendered in %.0f s.", elapsed());
        log(line);
    }

    const std::size_t trainCount = data.count - validation; // the last `validation` examples are held back
    if (trainCount < options.batchSize) {
        log("Not enough samples to train.");
        return result;
    }

    // 2. Standardise the inputs: zero mean and unit spread for each feature,
    //    measured on the training examples.
    std::vector<float> mean(data.inputs), scale(data.inputs);
    for (std::size_t f = 0; f < data.inputs; ++f) {
        double sum = 0.0, sumSquares = 0.0;
        for (std::size_t i = 0; i < trainCount; ++i) {
            const double v = data.features[i * data.inputs + f];
            sum += v;
            sumSquares += v * v;
        }
        const double m = sum / static_cast<double>(trainCount);
        const double variance = std::max(0.0, sumSquares / static_cast<double>(trainCount) - m * m);
        mean[f] = static_cast<float>(m);
        scale[f] = static_cast<float>(1.0 / std::max(std::sqrt(variance), 1.0e-3));
    }
    for (std::size_t i = 0; i < data.count; ++i)
        for (std::size_t f = 0; f < data.inputs; ++f)
            data.features[i * data.inputs + f] = (data.features[i * data.inputs + f] - mean[f]) * scale[f];

    // 3. Train.
    std::vector<std::size_t> sizes{data.inputs};
    for (int h = 0; h < options.hiddenLayers; ++h)
        sizes.push_back(options.hiddenSize);
    sizes.push_back(data.outputs);
    Mlp net = Mlp::create(sizes, options.seed);
    log("Training a network with " + std::to_string(net.parameterCount()) + " weights for " +
        std::to_string(options.epochs) + " epochs...");

    Adam adam(net);
    std::vector<Workspace> workspaces;
    workspaces.reserve(threads);
    for (unsigned t = 0; t < threads; ++t)
        workspaces.emplace_back(net);

    std::vector<std::size_t> order(trainCount);
    std::iota(order.begin(), order.end(), std::size_t{0});
    Rng shuffle(options.seed + 17u);

    const std::size_t batches = trainCount / options.batchSize;
    const std::size_t totalSteps = batches * static_cast<std::size_t>(std::max(1, options.epochs));
    std::size_t stepIndex = 0;

    for (int epoch = 0; epoch < options.epochs && !cancelled(); ++epoch) {
        for (std::size_t i = trainCount - 1; i > 0; --i)
            std::swap(order[i], order[static_cast<std::size_t>(shuffle.uniform() * static_cast<double>(i + 1))]);

        double epochLoss = 0.0;
        for (std::size_t b = 0; b < batches && !cancelled(); ++b, ++stepIndex) {
            // Each thread takes a slice of the batch and adds up its gradients.
            const std::size_t batchStart = b * options.batchSize;
            const std::size_t slices = std::min<std::size_t>(threads, options.batchSize);
            parallelFor(slices, threads, [&](std::size_t s) {
                Workspace& ws = workspaces[s];
                ws.clear();
                const std::size_t from = batchStart + s * options.batchSize / slices;
                const std::size_t to = batchStart + (s + 1) * options.batchSize / slices;
                for (std::size_t k = from; k < to; ++k) {
                    const std::size_t i = order[k];
                    accumulate(net, space, data.x(i), data.t(i), data.m(i), ws);
                }
            });
            Workspace& total = workspaces[0];
            for (std::size_t s = 1; s < slices; ++s) {
                total.loss += workspaces[s].loss;
                for (std::size_t l = 0; l < net.layers.size(); ++l) {
                    auto& gw = total.gradWeights[l];
                    const auto& ow = workspaces[s].gradWeights[l];
                    for (std::size_t k = 0; k < gw.size(); ++k)
                        gw[k] += ow[k];
                    auto& gb = total.gradBiases[l];
                    const auto& ob = workspaces[s].gradBiases[l];
                    for (std::size_t k = 0; k < gb.size(); ++k)
                        gb[k] += ob[k];
                }
            }
            epochLoss += total.loss;

            // The step size falls smoothly to a twentieth of its starting value.
            const double progress = static_cast<double>(stepIndex) / static_cast<double>(totalSteps);
            const double rate = options.learningRate * (0.05 + 0.95 * 0.5 * (1.0 + std::cos(3.141592653589793 * progress)));
            adam.step(net, total, rate, 1.0 / static_cast<double>(options.batchSize));
        }

        result.report.trainLoss = epochLoss / static_cast<double>(batches * options.batchSize);
        result.report.epochLoss.push_back(result.report.trainLoss);
        result.report.validationLoss = meanLoss(net, space, data, trainCount, data.count, threads);
        char line[160];
        std::snprintf(line, sizeof line, "epoch %d/%d: training loss %.3f, validation loss %.3f (%.0f s)", epoch + 1,
                      options.epochs, result.report.trainLoss, result.report.validationLoss, elapsed());
        log(line);
    }
    if (cancelled())
        return result;

    // 4. Measure each dimension on the held-back examples.
    auto& report = result.report;
    report.dimScore.assign(space.dims.size(), 0.0);
    report.dimCount.assign(space.dims.size(), 0);
    Workspace ws(net);
    for (std::size_t i = trainCount; i < data.count; ++i) {
        forward(net, data.x(i), ws);
        const float* out = ws.activations.back().data();
        const float* t = data.t(i);
        const float* m = data.m(i);
        std::size_t at = 0;
        for (std::size_t d = 0; d < space.dims.size(); ++d) {
            const auto& dim = space.dims[d];
            const std::size_t width = dim.isChoice() ? dim.choices.size() : 1;
            if (m[at] > 0.0f) {
                if (dim.isChoice()) {
                    std::size_t guess = 0, right = 0;
                    for (std::size_t c = 1; c < width; ++c) {
                        if (out[at + c] > out[at + guess])
                            guess = c;
                        if (t[at + c] > t[at + right])
                            right = c;
                    }
                    report.dimScore[d] += guess == right ? 1.0 : 0.0;
                } else {
                    report.dimScore[d] += std::abs(1.0 / (1.0 + std::exp(-static_cast<double>(out[at]))) - t[at]);
                }
                ++report.dimCount[d];
            }
            at += width;
        }
    }
    double errorSum = 0.0, accuracySum = 0.0;
    std::size_t errorDims = 0, accuracyDims = 0;
    for (std::size_t d = 0; d < space.dims.size(); ++d) {
        if (report.dimCount[d] == 0)
            continue;
        report.dimScore[d] /= static_cast<double>(report.dimCount[d]);
        if (space.dims[d].isChoice()) {
            accuracySum += report.dimScore[d];
            ++accuracyDims;
        } else {
            errorSum += report.dimScore[d];
            ++errorDims;
        }
    }
    report.meanError = errorDims > 0 ? errorSum / static_cast<double>(errorDims) : 0.0;
    report.meanAccuracy = accuracyDims > 0 ? accuracySum / static_cast<double>(accuracyDims) : 0.0;

    result.encoder = Encoder(std::move(net), std::move(mean), std::move(scale));
    result.ok = true;
    return result;
}

} // namespace synthmatch::match
