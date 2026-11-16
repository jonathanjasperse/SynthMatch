#include "ui/MainView.h"

#include "PluginProcessor.h"
#include "Session.h"
#include "synthmatch/dsp/MatchEq.h"
#include "ui/Backdrop.h"

#include <cmath>

namespace ui {

namespace {

const char* noteName(int note)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((note % 12) + 12) % 12];
}

juce::String duration(double seconds)
{
    return seconds < 60.0 ? juce::String(seconds, 1) + " s"
                          : juce::String(static_cast<int>(seconds) / 60) + ":" +
                                juce::String(static_cast<int>(seconds) % 60).paddedLeft('0', 2);
}

} // namespace

// ---------------------------------------------------------------- icon button

IconButton::IconButton(const juce::String& text, Icon icon) : juce::Button(text), icon_(icon)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

int IconButton::idealWidth() const
{
    const int textWidth = getButtonText().isEmpty()
                              ? 0
                              : juce::GlyphArrangement::getStringWidthInt(theme::caps(11.0f), getButtonText().toUpperCase()) + 8;
    const int iconWidth = icon_ == Icon::None ? 0 : 16;
    return 28 + textWidth + iconWidth + (textWidth > 0 && iconWidth > 0 ? 4 : 0);
}

void IconButton::drawIcon(juce::Graphics& g, Icon icon, juce::Rectangle<float> a, juce::Colour colour)
{
    g.setColour(colour);
    const auto c = a.getCentre();
    const float s = std::min(a.getWidth(), a.getHeight());
    juce::Path p;
    const juce::PathStrokeType stroke(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    switch (icon) {
    case Icon::Play:
        p.addTriangle(c.x - s * 0.28f, c.y - s * 0.34f, c.x - s * 0.28f, c.y + s * 0.34f, c.x + s * 0.36f, c.y);
        g.fillPath(p);
        break;
    case Icon::Stop:
        g.fillRoundedRectangle(juce::Rectangle<float>(s * 0.55f, s * 0.55f).withCentre(c), 1.5f);
        break;
    case Icon::Plus:
        g.drawLine(c.x - s * 0.36f, c.y, c.x + s * 0.36f, c.y, 1.6f);
        g.drawLine(c.x, c.y - s * 0.36f, c.x, c.y + s * 0.36f, 1.6f);
        break;
    case Icon::Refresh:
        p.addCentredArc(c.x, c.y, s * 0.32f, s * 0.32f, 0.0f, 0.6f, 5.6f, true);
        g.strokePath(p, stroke);
        p.clear();
        {
            const auto tip = c.getPointOnCircumference(s * 0.32f, 0.6f);
            p.addTriangle(tip.x - s * 0.16f, tip.y - s * 0.02f, tip.x + s * 0.14f, tip.y - s * 0.12f, tip.x + s * 0.02f, tip.y + s * 0.18f);
            g.fillPath(p);
        }
        break;
    case Icon::Open:
        p.startNewSubPath(c.x - s * 0.4f, c.y + s * 0.3f);
        p.lineTo(c.x - s * 0.4f, c.y - s * 0.3f);
        p.lineTo(c.x - s * 0.1f, c.y - s * 0.3f);
        p.lineTo(c.x, c.y - s * 0.18f);
        p.lineTo(c.x + s * 0.4f, c.y - s * 0.18f);
        p.lineTo(c.x + s * 0.4f, c.y + s * 0.3f);
        p.closeSubPath();
        g.strokePath(p, stroke);
        break;
    case Icon::Save:
        p.startNewSubPath(c.x, c.y - s * 0.38f);
        p.lineTo(c.x, c.y + s * 0.12f);
        g.strokePath(p, stroke);
        p.clear();
        p.startNewSubPath(c.x - s * 0.2f, c.y - s * 0.08f);
        p.lineTo(c.x, c.y + s * 0.12f);
        p.lineTo(c.x + s * 0.2f, c.y - s * 0.08f);
        g.strokePath(p, stroke);
        g.drawLine(c.x - s * 0.36f, c.y + s * 0.36f, c.x + s * 0.36f, c.y + s * 0.36f, 1.5f);
        break;
    case Icon::Back:
        p.startNewSubPath(c.x + s * 0.12f, c.y - s * 0.3f);
        p.lineTo(c.x - s * 0.18f, c.y);
        p.lineTo(c.x + s * 0.12f, c.y + s * 0.3f);
        g.strokePath(p, stroke);
        break;
    case Icon::Nodes:
        g.drawEllipse(juce::Rectangle<float>(s * 0.3f, s * 0.3f).withCentre({c.x - s * 0.28f, c.y + s * 0.2f}), 1.4f);
        g.drawEllipse(juce::Rectangle<float>(s * 0.3f, s * 0.3f).withCentre({c.x + s * 0.28f, c.y - s * 0.2f}), 1.4f);
        p.startNewSubPath(c.x - s * 0.13f, c.y + s * 0.2f);
        p.cubicTo(c.x, c.y + s * 0.2f, c.x, c.y - s * 0.2f, c.x + s * 0.13f, c.y - s * 0.2f);
        g.strokePath(p, juce::PathStrokeType(1.2f));
        break;
    case Icon::Info:
        g.drawEllipse(juce::Rectangle<float>(s * 0.8f, s * 0.8f).withCentre(c), 1.3f);
        g.fillEllipse(juce::Rectangle<float>(s * 0.12f, s * 0.12f).withCentre({c.x, c.y - s * 0.18f}));
        g.drawLine(c.x, c.y - s * 0.04f, c.x, c.y + s * 0.24f, 1.5f);
        break;
    case Icon::None:
        break;
    }
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    if (framed_)
        getLookAndFeel().drawButtonBackground(g, *this, {}, highlighted && isEnabled(), down);

    const auto colour = !isEnabled() ? theme::colour::textFaint : (highlighted || getToggleState() ? theme::colour::textHi : theme::colour::text);
    auto area = getLocalBounds().toFloat().reduced(14.0f, 0.0f);
    const auto label = getButtonText().toUpperCase();
    const float textWidth = label.isEmpty() ? 0.0f : static_cast<float>(juce::GlyphArrangement::getStringWidthInt(theme::caps(11.0f), label) + 4);
    const float iconWidth = icon_ == Icon::None ? 0.0f : 12.0f;
    const float gap = textWidth > 0.0f && iconWidth > 0.0f ? 8.0f : 0.0f;
    auto content = area.withSizeKeepingCentre(std::min(area.getWidth(), textWidth + iconWidth + gap), area.getHeight());
    if (iconWidth > 0.0f)
        drawIcon(g, icon_, content.removeFromLeft(iconWidth).withSizeKeepingCentre(iconWidth, iconWidth), colour);
    content.removeFromLeft(gap);
    g.setColour(colour);
    g.setFont(theme::caps(11.0f));
    g.drawText(label, content, juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------- main view

MainView::MainView(SynthMatchProcessor& processor, Session& session) : processor_(processor), session_(session)
{
    for (auto* b : {&playReference_, &playSynth_, &matchAgain_, &stop_, &choose_})
        addAndMakeVisible(*b);

    playReference_.onClick = [this] {
        if (processor_.isPlayingReference())
            session_.stopPlayback();
        else
            session_.playReference();
    };
    playSynth_.onClick = [this] {
        if (processor_.isPlayingNotes())
            session_.stopPlayback();
        else
            session_.playSynth();
    };
    matchAgain_.onClick = [this] { session_.startMatch(); };
    stop_.onClick = [this] { session_.stopMatch(); };
    choose_.onClick = [this] {
        if (onChooseFile)
            onChooseFile();
    };

    playReference_.setTooltip("Listen to the recording");
    playSynth_.setTooltip("Play the same notes through the synth");
    matchAgain_.setTooltip("Search again, from a different starting point");
    stop_.setTooltip("Stop searching and keep the closest sound so far");

    session_.addChangeListener(this);
    updateButtons();
}

MainView::~MainView() { session_.removeChangeListener(this); }

void MainView::setDragHover(bool hover)
{
    if (dragHover_ != hover) {
        dragHover_ = hover;
        repaint();
    }
}

void MainView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    updateButtons();
    repaint();
}

void MainView::updateButtons()
{
    const bool has = session_.hasReference();
    const bool running = session_.matchState() == Session::MatchState::Running;
    choose_.setVisible(!has);
    playReference_.setVisible(has);
    playSynth_.setVisible(has && !running);
    matchAgain_.setVisible(has && !running);
    stop_.setVisible(running);
    playReference_.setIcon(processor_.isPlayingReference() ? IconButton::Icon::Stop : IconButton::Icon::Play);
    playSynth_.setIcon(processor_.isPlayingNotes() ? IconButton::Icon::Stop : IconButton::Icon::Play);
    playSynth_.setButtonText(session_.lastMatch().valid ? "Synth" : "Preview");
}

void MainView::resized()
{
    auto area = getLocalBounds().reduced(20, 8);
    const int rightWidth = juce::jlimit(280, 380, area.getWidth() * 34 / 100);
    rightCard_ = area.removeFromRight(rightWidth).toFloat();
    area.removeFromRight(14);
    leftCard_ = area.toFloat();

    // Buttons along the bottom of the score card.
    auto buttons = rightCard_.toNearestInt().reduced(20, 18).removeFromBottom(34);
    const int half = (buttons.getWidth() - 10) / 2;
    playReference_.setBounds(buttons.removeFromLeft(half));
    buttons.removeFromLeft(10);
    playSynth_.setBounds(buttons);
    auto second = rightCard_.toNearestInt().reduced(20, 18).withTrimmedBottom(44).removeFromBottom(34);
    matchAgain_.setBounds(second);
    stop_.setBounds(second);

    choose_.setBounds(juce::Rectangle<int>(170, 36).withCentre(leftCard_.toNearestInt().getCentre().translated(0, 96)));
}

void MainView::mouseUp(const juce::MouseEvent& e)
{
    if (!session_.hasReference() && leftCard_.contains(e.position) && onChooseFile)
        onChooseFile();
}

void MainView::paint(juce::Graphics& g)
{
    theme::drawPanel(g, leftCard_, 14.0f, dragHover_ ? 0.85f : 0.58f);
    theme::drawPanel(g, rightCard_, 14.0f, 0.5f);

    if (dragHover_) {
        juce::Path outline;
        outline.addRoundedRectangle(leftCard_.reduced(6.0f), 10.0f);
        juce::Path dashed;
        const float dash[] = {6.0f, 5.0f};
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, outline, dash, 2);
        g.setColour(theme::colour::silverHi.withAlpha(0.6f));
        g.fillPath(dashed);
    }

    if (session_.hasReference())
        paintReference(g, leftCard_);
    else
        paintEmpty(g, leftCard_);
    paintScore(g, rightCard_);
}

void MainView::paintEmpty(juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto c = area.getCentre().translated(0.0f, -38.0f);
    // Your tile, at full strength and about as big as the drawn gem.
    constexpr float iconSize = 84.0f;
    const float pixelScale = static_cast<float>(g.getInternalContext().getPhysicalPixelScaleFactor());
    if (const auto tile = theme::tileIcon(juce::roundToInt(iconSize * pixelScale)); tile.isValid()) {
        g.drawImage(tile, juce::Rectangle<float>(iconSize, iconSize).withCentre(c), juce::RectanglePlacement::centred);
    } else {
        theme::drawGem(g, c, 36.0f, 1.0f, 0.9f);
    }

    g.setColour(theme::colour::textHi);
    g.setFont(theme::light(24.0f));
    g.drawText("Drop a synth recording here", area.withTrimmedTop(area.getHeight() * 0.5f).removeFromTop(34).translated(0.0f, 4.0f),
               juce::Justification::centred, false);
    g.setColour(theme::colour::textDim);
    g.setFont(theme::sans(13.0f));
    g.drawText("WAV, AIFF, FLAC, OGG or MP3. One instrument, a few notes, as dry as you have it.",
               area.withTrimmedTop(area.getHeight() * 0.5f + 40.0f).removeFromTop(20), juce::Justification::centred, false);
}

void MainView::paintReference(juce::Graphics& g, juce::Rectangle<float> card)
{
    const auto& ref = session_.reference();
    const auto& last = session_.lastMatch();
    auto area = card.reduced(22.0f, 18.0f);

    // Header.
    auto header = area.removeFromTop(22.0f);
    g.setFont(theme::caps(10.0f));
    g.setColour(theme::colour::textDim);
    g.drawText("REFERENCE", header, juce::Justification::centredLeft, false);
    g.setFont(theme::sans(12.0f));
    g.drawText(duration(ref.seconds) + juce::String::fromUTF8("  \xc2\xb7  ") +
                   juce::String(ref.audio ? ref.audio->sampleRate / 1000.0 : 0.0, 1) + " kHz",
               header, juce::Justification::centredRight, false);
    g.setFont(theme::light(22.0f));
    g.setColour(theme::colour::textHi);
    g.drawText(ref.file.getFileNameWithoutExtension(), area.removeFromTop(32.0f), juce::Justification::centredLeft, true);
    area.removeFromTop(10.0f);

    // Waveform, with the matched part lit.
    auto wave = area.removeFromTop(juce::jlimit(60.0f, 110.0f, area.getHeight() * 0.32f));
    const int points = Session::kOverviewPoints;
    const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
    float loudest = 1.0e-3f;
    for (const float v : ref.peaks)
        loudest = std::max(loudest, std::abs(v));
    juce::Path shape;
    for (int i = 0; i < points; ++i) {
        const float x = wave.getX() + wave.getWidth() * static_cast<float>(i) / static_cast<float>(points - 1);
        const float y = mid - half * ref.peaks[static_cast<std::size_t>(2 * i + 1)] / loudest;
        i == 0 ? shape.startNewSubPath(x, y) : shape.lineTo(x, y);
    }
    for (int i = points - 1; i >= 0; --i) {
        const float x = wave.getX() + wave.getWidth() * static_cast<float>(i) / static_cast<float>(points - 1);
        shape.lineTo(x, mid - half * ref.peaks[static_cast<std::size_t>(2 * i)] / loudest);
    }
    shape.closeSubPath();

    g.setColour(theme::colour::textFaint.withAlpha(0.5f));
    g.fillPath(shape);
    if (last.valid && ref.seconds > 0.0) {
        const float x0 = wave.getX() + wave.getWidth() * static_cast<float>(last.sectionStartSec / ref.seconds);
        const float x1 = wave.getX() + wave.getWidth() * static_cast<float>((last.sectionStartSec + last.sectionSeconds) / ref.seconds);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(juce::Rectangle<float>(x0, wave.getY() - 2.0f, std::max(1.0f, x1 - x0), wave.getHeight() + 4.0f).toNearestInt());
        g.setGradientFill(theme::silver(wave));
        g.fillPath(shape);
    } else {
        g.setGradientFill(theme::silver(wave));
        g.setOpacity(session_.matchState() == Session::MatchState::Running ? 0.6f : 0.9f);
        g.fillPath(shape);
    }
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawHorizontalLine(juce::roundToInt(mid), wave.getX(), wave.getRight());

    // Notes found.
    area.removeFromTop(8.0f);
    auto notesRow = area.removeFromTop(20.0f);
    if (last.valid && !last.notes.empty() && ref.seconds > 0.0) {
        g.setFont(theme::sans(11.5f));
        for (const auto& n : last.notes) {
            const float x = wave.getX() + wave.getWidth() * static_cast<float>((last.sectionStartSec + n.startSec) / ref.seconds);
            g.setColour(theme::colour::text);
            g.drawText(juce::String(noteName(n.note)) + juce::String(n.note / 12 - 1),
                       juce::Rectangle<float>(x, notesRow.getY(), 40.0f, notesRow.getHeight()), juce::Justification::centredLeft, false);
        }
    }

    area.removeFromTop(10.0f);
    paintSpectrum(g, area);
}

void MainView::paintSpectrum(juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& last = session_.lastMatch();
    auto header = area.removeFromTop(18.0f);
    g.setFont(theme::caps(10.0f));
    g.setColour(theme::colour::textDim);
    g.drawText("SPECTRUM", header, juce::Justification::centredLeft, false);

    // Legend.
    auto legend = header.removeFromRight(190.0f);
    const auto key = [&](juce::Rectangle<float> r, const juce::String& name, bool reference) {
        const float y = r.getCentreY();
        if (reference) {
            g.setGradientFill(theme::silver({r.getX(), y - 2, 16, 4}));
            g.drawLine(r.getX(), y, r.getX() + 16.0f, y, 2.0f);
        } else {
            g.setColour(theme::colour::textDim);
            const float dash[] = {3.0f, 3.0f};
            g.drawDashedLine({r.getX(), y, r.getX() + 16.0f, y}, dash, 2, 1.4f);
        }
        g.setColour(theme::colour::textDim);
        g.drawText(name, r.withTrimmedLeft(22.0f), juce::Justification::centredLeft, false);
    };
    key(legend.removeFromLeft(100.0f), "REFERENCE", true);
    key(legend, "SYNTH", false);

    area.removeFromTop(6.0f);
    const auto plot = area;
    if (plot.getHeight() < 30.0f)
        return;

    // Grid: 100 Hz, 1 kHz, 10 kHz.
    const auto xOf = [&](double hz) {
        const double t = (std::log2(hz) - std::log2(20.0)) / (std::log2(20000.0) - std::log2(20.0));
        return plot.getX() + plot.getWidth() * static_cast<float>(t);
    };
    g.setFont(theme::sans(10.5f));
    for (const auto& [hz, label] : {std::pair<double, const char*>{100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}}) {
        const float x = xOf(hz);
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom() - 14.0f);
        g.setColour(theme::colour::textFaint);
        g.drawText(label, juce::Rectangle<float>(x - 20.0f, plot.getBottom() - 13.0f, 40.0f, 13.0f), juce::Justification::centred, false);
    }
    const auto graph = plot.withTrimmedBottom(16.0f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawHorizontalLine(juce::roundToInt(graph.getBottom()), graph.getX(), graph.getRight());

    if (!last.hasSpectra) {
        g.setFont(theme::sans(12.5f));
        g.setColour(theme::colour::textFaint);
        g.drawText(session_.matchState() == Session::MatchState::Running ? "Comparing appears when the match is done"
                                                                         : "No comparison yet",
                   graph, juce::Justification::centred, false);
        return;
    }

    float top = -200.0f;
    for (int b = 0; b < synthmatch::kMatchEqBands; ++b)
        top = std::max({top, last.referenceSpectrum[static_cast<std::size_t>(b)], last.synthSpectrum[static_cast<std::size_t>(b)]});
    const float range = 60.0f;
    const auto curve = [&](const synthmatch::MatchEqGains& db) {
        juce::Path p;
        bool started = false;
        for (int b = 0; b < synthmatch::kMatchEqBands; ++b) {
            const double hz = synthmatch::dsp::matchEqBandHz(b);
            const float v = db[static_cast<std::size_t>(b)];
            if (hz > 20000.0 || v < -150.0f)
                continue;
            const float y = graph.getBottom() - graph.getHeight() * juce::jlimit(0.0f, 1.0f, (v - (top - range)) / range);
            const float x = xOf(hz);
            if (!started)
                p.startNewSubPath(x, y);
            else
                p.lineTo(x, y);
            started = true;
        }
        return p.createPathWithRoundedCorners(6.0f);
    };

    const auto ref = curve(last.referenceSpectrum);
    juce::Path fill(ref);
    fill.lineTo(graph.getRight(), graph.getBottom());
    fill.lineTo(graph.getX(), graph.getBottom());
    fill.closeSubPath();
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.10f), graph.getTopLeft(), juce::Colours::transparentWhite,
                                           graph.getBottomLeft(), false));
    g.fillPath(fill);
    g.setGradientFill(theme::silver(graph));
    g.strokePath(ref, juce::PathStrokeType(1.8f));

    juce::Path dashed;
    const float dash[] = {4.0f, 3.0f};
    juce::PathStrokeType(1.4f).createDashedStroke(dashed, curve(last.synthSpectrum), dash, 2);
    g.setColour(theme::colour::textDim);
    g.fillPath(dashed);
}

void MainView::paintScore(juce::Graphics& g, juce::Rectangle<float> card)
{
    auto area = card.reduced(22.0f, 18.0f);
    g.setFont(theme::caps(10.0f));
    g.setColour(theme::colour::textDim);
    g.drawText("MATCH", area.removeFromTop(22.0f), juce::Justification::centredLeft, false);

    const auto state = session_.matchState();
    const auto& last = session_.lastMatch();

    const auto bigNumber = [&](const juce::String& number, const juce::String& suffix) {
        auto row = area.removeFromTop(84.0f);
        g.setFont(theme::light(72.0f));
        const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), number);
        g.setGradientFill(theme::silver(row));
        g.drawText(number, row, juce::Justification::centredLeft, false);
        g.setFont(theme::light(22.0f));
        g.setColour(theme::colour::textDim);
        g.drawText(suffix, row.withTrimmedLeft(static_cast<float>(w + 6)).withTrimmedTop(36.0f), juce::Justification::topLeft, false);
    };
    const auto line = [&](const juce::String& text, juce::Colour colour, float size = 13.0f) {
        g.setFont(theme::sans(size));
        g.setColour(colour);
        g.drawFittedText(text, area.removeFromTop(size + 7.0f).toNearestInt(), juce::Justification::centredLeft, 2);
    };

    if (state == Session::MatchState::Running) {
        bigNumber(juce::String(juce::roundToInt(session_.matchProgress() * 100.0)), "%");
        // Progress line.
        auto bar = area.removeFromTop(10.0f).withHeight(2.0f).translated(0.0f, 4.0f);
        g.setColour(theme::colour::line);
        g.fillRect(bar);
        g.setGradientFill(theme::silver(bar.withHeight(8.0f)));
        g.fillRect(bar.withWidth(bar.getWidth() * static_cast<float>(session_.matchProgress())));
        area.removeFromTop(8.0f);
        line(session_.matchStage() + juce::String::fromUTF8("\xe2\x80\xa6"), theme::colour::text);
        if (session_.liveDistance() > 0.0 && session_.liveDistance() < 500.0)
            line("Closest so far: " + juce::String(juce::roundToInt(100.0 * std::exp(-session_.liveDistance() / 10.0))) + "/100",
                 theme::colour::textDim);
        line(session_.hasEncoder() ? "Starting from the trained encoder's guess, then searching on every CPU core but one."
                                   : "Searching on every CPU core but one.",
             theme::colour::textFaint, 12.0f);
        return;
    }

    if (last.valid) {
        bigNumber(juce::String(juce::roundToInt(last.score)), "/100");
        juce::StringArray notes;
        for (const auto& n : last.notes)
            notes.add(juce::String(noteName(n.note)) + juce::String(n.note / 12 - 1));
        line(juce::String(last.notes.size()) + (last.notes.size() == 1 ? " note: " : " notes: ") + notes.joinIntoString(" "),
             theme::colour::text);
        if (last.evaluations > 0)
            line(juce::String(last.evaluations) + " sounds tried in " + juce::String(last.seconds, 1) + " s", theme::colour::textDim);
        line(last.score >= 70 ? "Close. Fine-tune it with the assistant below." :
             last.score >= 45 ? "In the neighbourhood. Try Match again, or describe what's off below."
                              : "Rough. A dry recording of single notes works best.",
             theme::colour::textFaint, 12.0f);
        return;
    }

    if (state == Session::MatchState::Failed) {
        bigNumber(juce::String::fromUTF8("\xe2\x80\x94"), "");
        line(last.error, theme::colour::warn);
        return;
    }

    bigNumber(juce::String::fromUTF8("\xe2\x80\x94"), "");
    line("Add a recording to build a matching preset.", theme::colour::textDim);
    line("Press + or drop a file anywhere on the window.", theme::colour::textFaint, 12.0f);
}

} // namespace ui
