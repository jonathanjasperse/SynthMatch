#include "ui/NodeView.h"

#include "PluginProcessor.h"
#include "Session.h"
#include "synthmatch/assist/Edits.h"
#include "synthmatch/dsp/Filter.h"
#include "synthmatch/dsp/Lfo.h"
#include "synthmatch/dsp/MatchEq.h"
#include "synthmatch/dsp/Oscillator.h"

#include <cmath>

namespace ui {

namespace p = synthmatch::param;

namespace {

constexpr float kNodeW = 118.0f, kNodeH = 52.0f;
constexpr float kVirtualW = 948.0f, kVirtualH = 304.0f;

juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

} // namespace

// ---------------------------------------------------------------- nodes

struct NodeView::Node {
    juce::String key, title, about;
    std::vector<int> params;    // shown in the inspector
    int power = -1;             // the parameter the light switches
    juce::Point<float> pos;     // top-left, in the virtual canvas
    juce::Point<float> home;    // the default position
    juce::String target;        // where the output goes
    bool modulator = false;
};

std::vector<NodeView::Node> NodeView::makeNodes()
{
    std::vector<Node> n;
    const auto add = [&](const char* key, const char* title, const char* about, std::vector<int> params, int power,
                         float x, float y, const char* target, bool modulator = false) {
        n.push_back({key, title, about, std::move(params), power, {x, y}, {x, y}, target, modulator});
    };
    for (int k = 0; k < p::kNumOscs; ++k) {
        static const char* keys[] = {"osc1", "osc2", "osc3"};
        static const char* titles[] = {"OSC 1", "OSC 2", "OSC 3"};
        add(keys[k], titles[k], "A band-limited oscillator. Coarse and Fine tune it against the note being played.",
            {p::osc(k, p::OscWave), p::osc(k, p::OscCoarse), p::osc(k, p::OscFine), p::osc(k, p::OscPulseWidth),
             p::osc(k, p::OscLevel)},
            p::osc(k, p::OscLevel), 20.0f, 16.0f + 68.0f * static_cast<float>(k), "mixer");
    }
    add("noise", "NOISE", "White noise, for breath, air and texture.", {p::NoiseLevel}, p::NoiseLevel, 20.0f, 220.0f, "mixer");
    add("mixer", "MIXER", "Balances the sources, and stacks detuned unison copies of the oscillators across the stereo field.",
        {p::Osc1Level, p::Osc2Level, p::Osc3Level, p::NoiseLevel, p::UnisonVoices, p::UnisonDetune, p::UnisonSpread}, -1,
        178.0f, 118.0f, "filter");
    add("filter", "FILTER", "Shapes the brightness. Its envelope opens and closes it on every note; key tracking follows the pitch.",
        {p::FilterType, p::FilterCutoffHz, p::FilterResonance, p::FilterKeyTrack, p::FilterEnvAmount, p::FilterAttackMs,
         p::FilterDecayMs, p::FilterSustain, p::FilterReleaseMs},
        -1, 336.0f, 118.0f, "amp");
    add("amp", "AMP", "The volume envelope of each note.", {p::AmpAttackMs, p::AmpDecayMs, p::AmpSustain, p::AmpReleaseMs}, -1,
        494.0f, 118.0f, "eq");
    add("lfo1", "LFO 1", "Slow movement: vibrato (pitch), wobble (cutoff), tremolo (amp) or pulse-width sweeps.",
        {p::Lfo1Target, p::Lfo1Wave, p::Lfo1RateHz, p::Lfo1Depth}, p::Lfo1Target, 178.0f, 236.0f, "", true);
    add("lfo2", "LFO 2", "A second LFO, for a second kind of movement.", {p::Lfo2Target, p::Lfo2Wave, p::Lfo2RateHz, p::Lfo2Depth},
        p::Lfo2Target, 336.0f, 236.0f, "", true);
    add("eq", "EQ", "An adjustable EQ: low shelf, two bells and a high shelf. Separate from the match EQ.",
        {p::EqOn, p::EqLowFreqHz, p::EqLowGainDb, p::EqMid1FreqHz, p::EqMid1GainDb, p::EqMid1Q, p::EqMid2FreqHz, p::EqMid2GainDb,
         p::EqMid2Q, p::EqHighFreqHz, p::EqHighGainDb},
        p::EqOn, 652.0f, 16.0f, "delay");
    add("delay", "DELAY", "Stereo or ping-pong echoes, free or synced to the tempo, with filters and wobble in the feedback.",
        {p::DelayOn, p::DelayTimeMs, p::DelaySync, p::DelayFeedback, p::DelayMix, p::DelayMode, p::DelaySpread, p::DelayLowCutHz,
         p::DelayHighCutHz, p::DelayModRateHz, p::DelayModDepth},
        p::DelayOn, 652.0f, 84.0f, "reverb");
    add("reverb", "REVERB", "Room and space.",
        {p::ReverbOn, p::ReverbMix, p::ReverbSize, p::ReverbDecaySec, p::ReverbDamping, p::ReverbPredelayMs, p::ReverbWidth},
        p::ReverbOn, 652.0f, 152.0f, "matcheq");
    add("matcheq", "MATCH EQ", "The correction measured against the reference recording. Amount sets how much of it is applied.",
        {p::MatchEqAmount}, p::MatchEqAmount, 652.0f, 220.0f, "output");
    add("output", "OUTPUT", "Final level, and how far the pitch wheel bends.", {p::GainDb, p::BendRange}, -1, 810.0f, 118.0f, "");
    return n;
}

// ---------------------------------------------------------------- canvas

class NodeView::Canvas : public juce::Component {
public:
    explicit Canvas(NodeView& owner) : owner_(owner) {}

    juce::Rectangle<float> screenRect(const Node& n) const
    {
        return {origin_.x + n.pos.x * scale_, origin_.y + n.pos.y * scale_, kNodeW * scale_, kNodeH * scale_};
    }

    void resized() override
    {
        const float pad = 10.0f;
        scale_ = std::min((static_cast<float>(getWidth()) - 2 * pad) / kVirtualW, (static_cast<float>(getHeight()) - 2 * pad) / kVirtualH);
        scale_ = juce::jlimit(0.5f, 1.6f, scale_);
        origin_ = {(static_cast<float>(getWidth()) - kVirtualW * scale_) * 0.5f, (static_cast<float>(getHeight()) - kVirtualH * scale_) * 0.5f};
    }

    void paint(juce::Graphics& g) override
    {
        // Wires first, under the nodes.
        for (const auto& n : owner_.nodes_) {
            if (n.target.isNotEmpty())
                if (const auto* to = find(n.target))
                    drawWire(g, n, *to, false, isOn(n) ? 1.0f : 0.25f, {});
            if (n.modulator && isOn(n))
                if (const auto* to = find(modTarget(n)))
                    drawWire(g, n, *to, true, 0.8f, modLabel(n));
        }
        for (const auto& n : owner_.nodes_)
            drawNode(g, n);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging_ = nullptr;
        for (auto it = owner_.nodes_.rbegin(); it != owner_.nodes_.rend(); ++it) {
            const auto r = screenRect(*it);
            if (!r.contains(e.position))
                continue;
            if (it->power >= 0 && lightRect(r).expanded(5.0f).contains(e.position)) {
                togglePower(*it);
                return;
            }
            dragging_ = &*it;
            dragStart_ = it->pos;
            owner_.select(it->key);
            return;
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!dragging_)
            return;
        auto pos = dragStart_ + e.getOffsetFromDragStart().toFloat() / scale_;
        pos.x = juce::jlimit(0.0f, kVirtualW - kNodeW, pos.x);
        pos.y = juce::jlimit(0.0f, kVirtualH - kNodeH, pos.y);
        dragging_->pos = pos;
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_)
            owner_.saveLayout();
        dragging_ = nullptr;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        // Double-click empty space to tidy the layout.
        for (const auto& n : owner_.nodes_)
            if (screenRect(n).contains(e.position))
                return;
        for (auto& n : owner_.nodes_)
            n.pos = n.home;
        owner_.saveLayout();
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        juce::String over;
        for (const auto& n : owner_.nodes_)
            if (screenRect(n).contains(e.position))
                over = n.key;
        if (over != hover_) {
            hover_ = over;
            setMouseCursor(over.isEmpty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
            repaint();
        }
    }

private:
    float value(int index) const { return owner_.processor_.currentParams()[static_cast<std::size_t>(index)]; }
    juce::String text(int index) const { return str(synthmatch::assist::formatValue(index, value(index))); }

    const Node* find(const juce::String& key) const
    {
        for (const auto& n : owner_.nodes_)
            if (n.key == key)
                return &n;
        return nullptr;
    }

    bool isOn(const Node& n) const
    {
        if (n.power < 0)
            return true;
        return value(n.power) > 0.0f;
    }

    juce::String modTarget(const Node& n) const
    {
        const auto target = static_cast<synthmatch::dsp::LfoTarget>(static_cast<int>(value(n.power)));
        switch (target) {
        case synthmatch::dsp::LfoTarget::Pitch:
        case synthmatch::dsp::LfoTarget::PulseWidth:
            return "mixer";
        case synthmatch::dsp::LfoTarget::Cutoff:
            return "filter";
        case synthmatch::dsp::LfoTarget::Amp:
            return "amp";
        default:
            return {};
        }
    }

    juce::String modLabel(const Node& n) const { return text(n.power).toUpperCase(); }

    static juce::Rectangle<float> lightRect(juce::Rectangle<float> r)
    {
        return {r.getRight() - 17.0f, r.getY() + 9.0f, 8.0f, 8.0f};
    }

    void togglePower(const Node& n)
    {
        auto& session = owner_.session_;
        const float now = value(n.power);
        float next = 0.0f;
        if (now > 0.0f) {
            remembered_[n.power] = now;
        } else {
            const auto it = remembered_.find(n.power);
            if (it != remembered_.end())
                next = it->second;
            else if (n.modulator)
                next = static_cast<float>(synthmatch::dsp::LfoTarget::Pitch);
            else if (n.key.startsWith("osc"))
                next = 0.7f;
            else if (n.key == "noise")
                next = 0.2f;
            else
                next = 1.0f;
        }
        session.applyEdits({{n.power, synthmatch::assist::Edit::Op::Set, next}}, (next > 0.0f ? "turn on " : "turn off ") + n.title.toLowerCase());
        repaint();
    }

    void drawWire(juce::Graphics& g, const Node& from, const Node& to, bool modulation, float strength, const juce::String& label)
    {
        const auto a = screenRect(from), b = screenRect(to);
        juce::Point<float> start, end, c1, c2;
        const bool stacked = std::abs(a.getX() - b.getX()) < a.getWidth() * 0.5f;
        if (modulation) {
            start = {a.getCentreX(), a.getY()};
            end = {b.getCentreX(), b.getBottom()};
            c1 = start.translated(0.0f, -40.0f * scale_);
            c2 = end.translated(0.0f, 40.0f * scale_);
        } else if (stacked) {
            const bool down = b.getY() > a.getY();
            start = {a.getCentreX(), down ? a.getBottom() : a.getY()};
            end = {b.getCentreX(), down ? b.getY() : b.getBottom()};
            const float bend = std::abs(end.y - start.y) * 0.5f * (down ? 1.0f : -1.0f);
            c1 = start.translated(0.0f, bend);
            c2 = end.translated(0.0f, -bend);
        } else {
            start = {a.getRight(), a.getCentreY()};
            end = {b.getX(), b.getCentreY()};
            const float bend = std::max(30.0f * scale_, std::abs(end.x - start.x) * 0.5f);
            c1 = start.translated(bend, 0.0f);
            c2 = end.translated(-bend, 0.0f);
        }

        juce::Path wire;
        wire.startNewSubPath(start);
        wire.cubicTo(c1, c2, end);

        g.setColour(juce::Colours::white.withAlpha(0.035f * strength));
        g.strokePath(wire, juce::PathStrokeType(5.0f));
        juce::ColourGradient metal(theme::colour::silverHi.withAlpha(0.6f * strength), start,
                                   theme::colour::silverLo.withAlpha(0.35f * strength), end, false);
        g.setGradientFill(metal);
        if (modulation) {
            juce::Path dashed;
            const float dash[] = {4.0f, 4.0f};
            juce::PathStrokeType(1.3f).createDashedStroke(dashed, wire, dash, 2);
            g.fillPath(dashed);
            if (label.isNotEmpty()) {
                const auto mid = wire.getPointAlongPath(wire.getLength() * 0.5f);
                g.setFont(theme::caps(8.5f));
                g.setColour(theme::colour::textDim);
                g.drawText(label, juce::Rectangle<float>(80.0f, 12.0f).withCentre(mid.translated(30.0f, 0.0f)),
                           juce::Justification::centredLeft, false);
            }
        } else {
            g.strokePath(wire, juce::PathStrokeType(1.5f));
        }

        // Ports.
        for (const auto& port : {start, end}) {
            g.setColour(juce::Colour(0xff0b0c0e));
            g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(port));
            g.setColour(theme::colour::silverLo.withAlpha(0.8f * std::max(strength, 0.4f)));
            g.drawEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(port), 1.0f);
        }
    }

    void drawNode(juce::Graphics& g, const Node& n)
    {
        const auto r = screenRect(n);
        const bool selected = n.key == owner_.selected_;
        const bool hover = n.key == hover_;
        const bool on = isOn(n);

        juce::Path body;
        body.addRoundedRectangle(r, 9.0f);
        juce::DropShadow(juce::Colours::black.withAlpha(0.6f), 14, {0, 5}).drawForPath(g, body);
        g.setGradientFill(juce::ColourGradient(juce::Colour(selected ? 0xff2d2f34 : (hover ? 0xff27292d : 0xff222327)), r.getTopLeft(),
                                               juce::Colour(0xff141518), r.getBottomLeft(), false));
        g.fillPath(body);
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(selected ? 0.55f : 0.18f), r.getTopLeft(),
                                               juce::Colours::white.withAlpha(selected ? 0.12f : 0.03f), r.getBottomLeft(), false));
        g.strokePath(body, juce::PathStrokeType(selected ? 1.3f : 1.0f));

        const float alpha = on ? 1.0f : 0.45f;
        auto inner = r.reduced(11.0f, 8.0f);
        g.setFont(theme::caps(10.0f));
        g.setColour(theme::colour::textHi.withAlpha(alpha));
        g.drawText(n.title, inner.removeFromTop(16.0f), juce::Justification::centredLeft, false);
        g.setFont(theme::sans(11.5f));
        g.setColour(theme::colour::textDim.withAlpha(alpha));
        g.drawText(summary(n), inner.withTrimmedRight(30.0f), juce::Justification::bottomLeft, true);

        drawGlyph(g, n, juce::Rectangle<float>(r.getRight() - 40.0f, r.getBottom() - 26.0f, 30.0f, 16.0f), alpha);

        if (n.power >= 0) {
            const auto light = lightRect(r);
            if (on) {
                g.setColour(theme::colour::silverHi.withAlpha(0.18f));
                g.fillEllipse(light.expanded(3.0f));
                g.setGradientFill(juce::ColourGradient(juce::Colours::white, light.getTopLeft(), theme::colour::silverLo,
                                                       light.getBottomRight(), false));
                g.fillEllipse(light);
            } else {
                g.setColour(juce::Colour(0xff0b0c0e));
                g.fillEllipse(light);
                g.setColour(theme::colour::lineHi);
                g.drawEllipse(light, 1.0f);
            }
        }
    }

    juce::String summary(const Node& n) const
    {
        const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
        const auto& key = n.key;
        if (key.startsWith("osc")) {
            const int k = key.getLastCharacter() - '1';
            const float coarse = value(p::osc(k, p::OscCoarse));
            return text(p::osc(k, p::OscWave)) + dot + (coarse > 0 ? "+" : "") + text(p::osc(k, p::OscCoarse)) + dot +
                   text(p::osc(k, p::OscLevel));
        }
        if (key == "noise")
            return text(p::NoiseLevel);
        if (key == "mixer") {
            const int voices = static_cast<int>(value(p::UnisonVoices));
            return voices > 1 ? juce::String(voices) + " voices" + dot + text(p::UnisonDetune) : juce::String("1 voice");
        }
        if (key == "filter")
            return text(p::FilterType) + dot + text(p::FilterCutoffHz);
        if (key == "amp")
            return "A " + text(p::AmpAttackMs) + dot + "R " + text(p::AmpReleaseMs);
        if (n.modulator) {
            if (value(n.power) == 0.0f)
                return "off";
            const int depth = n.key == "lfo1" ? p::Lfo1Depth : p::Lfo2Depth;
            const int rate = n.key == "lfo1" ? p::Lfo1RateHz : p::Lfo2RateHz;
            return text(rate) + dot + text(depth);
        }
        if (key == "eq") {
            if (value(p::EqOn) == 0.0f)
                return "off";
            return "L " + text(p::EqLowGainDb) + dot + "H " + text(p::EqHighGainDb);
        }
        if (key == "delay") {
            if (value(p::DelayOn) == 0.0f)
                return "off";
            return (value(p::DelaySync) > 0.0f ? text(p::DelaySync) : text(p::DelayTimeMs)) + dot + text(p::DelayMix);
        }
        if (key == "reverb") {
            if (value(p::ReverbOn) == 0.0f)
                return "off";
            return text(p::ReverbDecaySec) + dot + text(p::ReverbMix);
        }
        if (key == "matcheq") {
            bool any = false;
            for (const float g : owner_.processor_.matchEq())
                any = any || g != 0.0f;
            return any ? text(p::MatchEqAmount) : juce::String("flat");
        }
        if (key == "output")
            return text(p::GainDb);
        return {};
    }

    // A tiny picture of what the node is doing.
    void drawGlyph(juce::Graphics& g, const Node& n, juce::Rectangle<float> a, float alpha)
    {
        juce::Path path;
        const auto x = [&](float t) { return a.getX() + a.getWidth() * t; };
        const auto y = [&](float v) { return a.getCentreY() - a.getHeight() * 0.5f * v; };
        const int steps = 40;

        if (n.key.startsWith("osc") || n.modulator) {
            int waveIndex = 0;
            float pw = 0.5f;
            if (n.modulator) {
                waveIndex = static_cast<int>(value(n.key == "lfo1" ? p::Lfo1Wave : p::Lfo2Wave));
            } else {
                const int k = n.key.getLastCharacter() - '1';
                waveIndex = static_cast<int>(value(p::osc(k, p::OscWave)));
                pw = value(p::osc(k, p::OscPulseWidth));
            }
            for (int i = 0; i <= steps; ++i) {
                const float t = static_cast<float>(i) / steps;
                const float ph = std::fmod(t * 2.0f, 1.0f);
                float v = 0.0f;
                switch (waveIndex) {
                case 0: v = std::sin(ph * juce::MathConstants<float>::twoPi); break;
                case 1: v = ph < 0.25f ? 4 * ph : (ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4); break;
                case 2: v = 2 * ph - 1; break;
                case 3: v = ph < pw ? 1.0f : -1.0f; break;
                default: v = std::sin(std::floor(ph * 4.0f + t * 7.0f) * 2.3f); break; // random steps
                }
                i == 0 ? path.startNewSubPath(x(t), y(v * 0.8f)) : path.lineTo(x(t), y(v * 0.8f));
            }
        } else if (n.key == "noise") {
            juce::Random random(3);
            for (int i = 0; i <= steps; ++i) {
                const float t = static_cast<float>(i) / steps;
                const float v = (random.nextFloat() * 2.0f - 1.0f) * 0.8f;
                i == 0 ? path.startNewSubPath(x(t), y(v)) : path.lineTo(x(t), y(v));
            }
        } else if (n.key == "filter") {
            const auto type = static_cast<synthmatch::dsp::FilterType>(static_cast<int>(value(p::FilterType)));
            const float cut = synthmatch::toNormalized(synthmatch::spec(p::FilterCutoffHz), value(p::FilterCutoffHz));
            const float q = 1.0f + 3.0f * value(p::FilterResonance);
            for (int i = 0; i <= steps; ++i) {
                const float t = static_cast<float>(i) / steps;
                const float w = std::pow(2.0f, (t - cut) * 8.0f);
                const float den = std::sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q)) + 1e-3f;
                float gain = type == synthmatch::dsp::FilterType::LowPass ? 1 / den
                             : type == synthmatch::dsp::FilterType::HighPass ? w * w / den
                                                                           : (w / q) / den;
                const float db = juce::jlimit(-30.0f, 12.0f, 20.0f * std::log10(gain + 1e-4f));
                const float v = (db + 9.0f) / 21.0f;
                i == 0 ? path.startNewSubPath(x(t), y(v)) : path.lineTo(x(t), y(v));
            }
        } else if (n.key == "amp") {
            const auto& s = synthmatch::spec(p::AmpAttackMs);
            const float at = 0.05f + 0.25f * synthmatch::toNormalized(s, value(p::AmpAttackMs));
            const float dt = 0.05f + 0.25f * synthmatch::toNormalized(synthmatch::spec(p::AmpDecayMs), value(p::AmpDecayMs));
            const float rt = 0.05f + 0.25f * synthmatch::toNormalized(synthmatch::spec(p::AmpReleaseMs), value(p::AmpReleaseMs));
            const float sus = value(p::AmpSustain);
            path.startNewSubPath(x(0), y(-0.9f));
            path.lineTo(x(at), y(0.9f));
            path.lineTo(x(at + dt), y(-0.9f + 1.8f * sus));
            path.lineTo(x(1.0f - rt), y(-0.9f + 1.8f * sus));
            path.lineTo(x(1.0f), y(-0.9f));
        } else if (n.key == "matcheq") {
            const auto gains = owner_.processor_.matchEq();
            const float amount = value(p::MatchEqAmount);
            for (int b = 0; b < synthmatch::kMatchEqBands; ++b) {
                const float t = static_cast<float>(b) / (synthmatch::kMatchEqBands - 1);
                const float v = juce::jlimit(-1.0f, 1.0f, gains[static_cast<std::size_t>(b)] * amount / 9.0f);
                b == 0 ? path.startNewSubPath(x(t), y(v)) : path.lineTo(x(t), y(v));
            }
        } else {
            return;
        }
        g.setColour(theme::colour::silverLo.withAlpha(0.9f * alpha));
        g.strokePath(path, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    NodeView& owner_;
    float scale_ = 1.0f;
    juce::Point<float> origin_;
    Node* dragging_ = nullptr;
    juce::Point<float> dragStart_;
    juce::String hover_;
    std::map<int, float> remembered_;
};

// ---------------------------------------------------------------- inspector

class NodeView::Inspector : public juce::Component {
public:
    explicit Inspector(NodeView& owner) : owner_(owner) {}

    void show(const Node* node)
    {
        controls_.clear();
        node_ = node;
        if (!node) {
            repaint();
            return;
        }
        for (const int index : node->params) {
            const auto& s = synthmatch::spec(index);
            auto& parameter = owner_.processor_.parameter(index);
            const juce::String name = label(index);
            std::unique_ptr<juce::Component> c;
            if (s.choices.size() == 2 && s.choices[0] == "off")
                c = std::make_unique<theme::Switch>(parameter, name);
            else if (!s.choices.empty())
                c = std::make_unique<theme::Choice>(parameter, index, name);
            else
                c = std::make_unique<theme::Knob>(parameter, index, name);
            addAndMakeVisible(*c);
            controls_.push_back(std::move(c));
        }
        resized();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18, 12);
        area.removeFromLeft(kTitleWidth);
        if (controls_.empty())
            return;
        const int each = std::min(84, area.getWidth() / static_cast<int>(controls_.size()));
        for (auto& c : controls_) {
            const bool wide = dynamic_cast<theme::Choice*>(c.get()) != nullptr;
            c->setBounds(area.removeFromLeft(wide ? std::max(each, 92) : each).withSizeKeepingCentre(wide ? std::max(each, 92) - 8 : each, std::min(area.getHeight(), 92)));
        }
    }

    void paint(juce::Graphics& g) override
    {
        theme::drawPanel(g, getLocalBounds().toFloat(), 14.0f, 0.82f);
        if (!node_)
            return;
        auto area = getLocalBounds().reduced(20, 16).removeFromLeft(kTitleWidth - 16);
        g.setFont(theme::light(22.0f));
        g.setColour(theme::colour::textHi);
        g.drawText(node_->title.substring(0, 1) + node_->title.substring(1).toLowerCase(), area.removeFromTop(30),
                   juce::Justification::topLeft, true);
        g.setFont(theme::sans(12.0f));
        g.setColour(theme::colour::textDim);
        g.drawFittedText(node_->about, area.removeFromTop(64), juce::Justification::topLeft, 4, 1.0f);
    }

private:
    static constexpr int kTitleWidth = 200;

    static juce::String label(int index)
    {
        // The parameter's name without its node, e.g. "Filter Cutoff" -> "Cutoff".
        juce::String name = str(std::string(synthmatch::spec(index).name));
        for (const char* prefix : {"Osc 1 ", "Osc 2 ", "Osc 3 ", "LFO 1 ", "LFO 2 ", "Filter ", "Amp ", "EQ ", "Delay ", "Reverb ",
                                   "Unison ", "Match EQ "})
            if (name.startsWith(prefix) && name.length() > juce::String(prefix).length())
                return name.substring(juce::String(prefix).length());
        if (name == "Reverb" || name == "Delay" || name == "EQ")
            return "On";
        if (name.startsWith("Osc ") || name == "Noise Level")
            return name.replace(" Level", "");
        return name;
    }

    NodeView& owner_;
    const Node* node_ = nullptr;
    std::vector<std::unique_ptr<juce::Component>> controls_;
};

// ---------------------------------------------------------------- view

NodeView::NodeView(SynthMatchProcessor& processor, Session& session) : processor_(processor), session_(session)
{
    nodes_ = makeNodes();
    loadLayout();
    canvas_ = std::make_unique<Canvas>(*this);
    inspector_ = std::make_unique<Inspector>(*this);
    addAndMakeVisible(*canvas_);
    addAndMakeVisible(*inspector_);
    select(selected_);
    session_.addChangeListener(this);
    startTimerHz(15);
}

NodeView::~NodeView()
{
    session_.removeChangeListener(this);
}

void NodeView::select(const juce::String& key)
{
    selected_ = key;
    for (const auto& n : nodes_)
        if (n.key == key)
            inspector_->show(&n);
    canvas_->repaint();
}

void NodeView::saveLayout()
{
    juce::StringArray parts;
    for (const auto& n : nodes_)
        if (n.pos != n.home)
            parts.add(n.key + "=" + juce::String(n.pos.x, 1) + "," + juce::String(n.pos.y, 1));
    session_.nodeLayout = parts.joinIntoString(";");
}

void NodeView::loadLayout()
{
    juce::StringArray parts;
    parts.addTokens(session_.nodeLayout, ";", "");
    for (const auto& part : parts) {
        const auto key = part.upToFirstOccurrenceOf("=", false, false);
        const auto xy = part.fromFirstOccurrenceOf("=", false, false);
        for (auto& n : nodes_)
            if (n.key == key)
                n.pos = {juce::jlimit(0.0f, kVirtualW - kNodeW, xy.upToFirstOccurrenceOf(",", false, false).getFloatValue()),
                         juce::jlimit(0.0f, kVirtualH - kNodeH, xy.fromFirstOccurrenceOf(",", false, false).getFloatValue())};
    }
}

void NodeView::changeListenerCallback(juce::ChangeBroadcaster*) { canvas_->repaint(); }

void NodeView::timerCallback()
{
    // Repaint the summaries when a parameter changes, however it changed.
    const auto params = processor_.currentParams();
    bool changed = false;
    for (std::size_t i = 0; i < params.size() && i < lastParams_.size(); ++i) {
        changed = changed || params[i] != lastParams_[i];
        lastParams_[i] = params[i];
    }
    if (changed)
        canvas_->repaint();
}

void NodeView::resized()
{
    auto area = getLocalBounds().reduced(20, 8);
    inspector_->setBounds(area.removeFromBottom(132));
    area.removeFromBottom(10);
    canvas_->setBounds(area);
}

void NodeView::paint(juce::Graphics& g)
{
    // A faint dotted grid behind the nodes.
    const auto area = canvas_->getBounds().toFloat();
    g.setColour(juce::Colours::white.withAlpha(0.035f));
    for (float y = area.getY() + 12.0f; y < area.getBottom(); y += 24.0f)
        for (float x = area.getX() + 12.0f; x < area.getRight(); x += 24.0f)
            g.fillRect(x, y, 1.2f, 1.2f);
}

} // namespace ui
