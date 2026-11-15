#include "ui/Theme.h"

#include "synthmatch/Params.h"
#include "synthmatch/assist/Edits.h"

#include <cmath>

namespace theme {

namespace {

#if JUCE_MAC
const char* const kSans = "Helvetica Neue";
const char* const kLight = "Light";
const char* const kMedium = "Medium";
const char* const kMono = "Menlo";
#else
const char* const kSans = "Segoe UI";
const char* const kLight = "Light";
const char* const kMedium = "Semibold";
const char* const kMono = "Consolas";
#endif

juce::Font font(const char* style, float size, float tracking = 0.0f)
{
    return juce::Font(juce::FontOptions(kSans, style, size).withKerningFactor(tracking));
}

} // namespace

juce::Font sans(float size) { return font("Regular", size); }
juce::Font light(float size) { return font(kLight, size); }
juce::Font medium(float size) { return font(kMedium, size); }
juce::Font caps(float size) { return font(kMedium, size, 0.16f); }
juce::Font mono(float size) { return juce::Font(juce::FontOptions(kMono, size, juce::Font::plain)); }

juce::ColourGradient silver(juce::Rectangle<float> area)
{
    juce::ColourGradient g(colour::silverHi, area.getTopLeft(), colour::silverLo, area.getBottomLeft(), false);
    g.addColour(0.45, juce::Colour(0xffd5d8dd));
    g.addColour(0.55, juce::Colour(0xffa9aeb6));
    return g;
}

void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, float radius, float opacity)
{
    juce::Path shape;
    shape.addRoundedRectangle(area, radius);

    juce::DropShadow(juce::Colours::black.withAlpha(0.45f * opacity), 18, {0, 8}).drawForPath(g, shape);

    // Smoked glass: darker at the bottom, with a faint sheen at the top.
    juce::ColourGradient fill(juce::Colour(0xff1a1b1f).withAlpha(opacity), area.getTopLeft(),
                              juce::Colour(0xff0f1012).withAlpha(opacity), area.getBottomLeft(), false);
    g.setGradientFill(fill);
    g.fillPath(shape);

    juce::ColourGradient sheen(juce::Colours::white.withAlpha(0.045f), area.getTopLeft(), juce::Colours::transparentWhite,
                               {area.getX(), area.getY() + std::min(60.0f, area.getHeight() * 0.4f)}, false);
    g.setGradientFill(sheen);
    g.fillPath(shape);

    // A machined edge: light along the top, nearly gone at the bottom.
    juce::ColourGradient edge(juce::Colours::white.withAlpha(0.16f), area.getTopLeft(), juce::Colours::white.withAlpha(0.03f),
                              area.getBottomLeft(), false);
    g.setGradientFill(edge);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
}

void drawMark(juce::Graphics& g, juce::Rectangle<float> area)
{
    // A polished metal waveform: a short wave packet that swells and fades.
    juce::Path wave;
    const int steps = 64;
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        // max(0, ...): sin(pi) comes out a hair below zero, and pow of that is NaN.
        const float envelope = std::pow(std::max(0.0f, std::sin(juce::MathConstants<float>::pi * t)), 0.7f);
        const float v = std::sin(juce::MathConstants<float>::twoPi * 2.5f * t) * envelope;
        const juce::Point<float> p{area.getX() + area.getWidth() * t, area.getCentreY() - area.getHeight() * 0.42f * v};
        i == 0 ? wave.startNewSubPath(p) : wave.lineTo(p);
    }
    const float thickness = std::max(2.0f, area.getHeight() * 0.15f);
    const juce::PathStrokeType stroke(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    // A shadow beneath, the metal body, then a thin highlight along its top edge.
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.strokePath(wave, stroke, juce::AffineTransform::translation(0.0f, 1.2f));

    juce::ColourGradient body(juce::Colour(0xfff4f5f7), area.getTopLeft(), juce::Colour(0xff5c6168), area.getBottomLeft(), false);
    body.addColour(0.42, juce::Colour(0xffc9cdd3));
    body.addColour(0.55, juce::Colour(0xff7d828a));
    body.addColour(0.7, juce::Colour(0xffaeb3ba));
    g.setGradientFill(body);
    g.strokePath(wave, stroke);

    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.strokePath(wave, juce::PathStrokeType(thickness * 0.28f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 juce::AffineTransform::translation(0.0f, -thickness * 0.22f));
}

// ---------------------------------------------------------------- look and feel

MetalLookAndFeel::MetalLookAndFeel()
{
    using namespace colour;
    setColour(juce::ResizableWindow::backgroundColourId, bg);
    setColour(juce::Label::textColourId, text);
    setColour(juce::ComboBox::backgroundColourId, panelHi);
    setColour(juce::ComboBox::textColourId, textHi);
    setColour(juce::ComboBox::outlineColourId, lineHi);
    setColour(juce::ComboBox::arrowColourId, textDim);
    setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff17181b));
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff2b2d32));
    setColour(juce::PopupMenu::highlightedTextColourId, textHi);
    setColour(juce::TextEditor::textColourId, textHi);
    setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff3a3d44));
    setColour(juce::TextEditor::highlightedTextColourId, textHi);
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, silverHi);
    setColour(juce::ScrollBar::thumbColourId, lineHi);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, textHi);
    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff1b1c20));
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, lineHi);
}

void MetalLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                               static_cast<float>(height))
                            .reduced(2.0f);
    const float size = std::min(bounds.getWidth(), bounds.getHeight());
    const auto area = bounds.withSizeKeepingCentre(size, size);
    const auto c = area.getCentre();
    const float r = size * 0.5f - 1.0f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const bool hover = slider.isMouseOverOrDragging() && slider.isEnabled();

    // Track and value arcs.
    juce::Path track;
    track.addCentredArc(c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
    g.setColour(colour::line);
    g.strokePath(track, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    float from = startAngle;
    if (slider.getProperties()["bipolar"]) {
        const double zero = slider.valueToProportionOfLength(0.0);
        from = startAngle + static_cast<float>(zero) * (endAngle - startAngle);
    }
    if (std::abs(angle - from) > 0.01f) {
        juce::Path value;
        value.addCentredArc(c.x, c.y, r, r, 0.0f, std::min(from, angle), std::max(from, angle), true);
        g.setGradientFill(silver(area));
        g.strokePath(value, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The knob: a machined metal cap.
    const float rb = r - 5.0f;
    juce::Path cap;
    cap.addEllipse(c.x - rb, c.y - rb, 2.0f * rb, 2.0f * rb);
    juce::DropShadow(juce::Colours::black.withAlpha(0.7f), 7, {0, 3}).drawForPath(g, cap);

    juce::ColourGradient body(juce::Colour(hover ? 0xff45484f : 0xff3a3d43), {c.x - rb * 0.45f, c.y - rb * 0.6f},
                              juce::Colour(0xff111214), {c.x + rb * 0.5f, c.y + rb}, true);
    g.setGradientFill(body);
    g.fillPath(cap);

    // A soft specular band across the top.
    juce::ColourGradient spec(juce::Colours::white.withAlpha(0.16f), {c.x, c.y - rb}, juce::Colours::transparentWhite,
                              {c.x, c.y + rb * 0.1f}, false);
    g.setGradientFill(spec);
    g.fillEllipse(c.x - rb * 0.8f, c.y - rb * 0.95f, rb * 1.6f, rb * 1.1f);

    juce::ColourGradient rim(juce::Colour(0xff70747c), {c.x, c.y - rb}, juce::Colour(0xff0c0d0f), {c.x, c.y + rb}, false);
    g.setGradientFill(rim);
    g.strokePath(cap, juce::PathStrokeType(1.0f));

    // Indicator.
    const juce::Point<float> inner = c.getPointOnCircumference(rb * 0.3f, angle);
    const juce::Point<float> outer = c.getPointOnCircumference(rb * 0.82f, angle);
    g.setColour(slider.isEnabled() ? colour::silverHi : colour::textFaint);
    g.drawLine({inner, outer}, 2.0f);
}

void MetalLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted,
                                            bool down)
{
    const auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = area.getHeight() * 0.5f;
    const bool on = button.getToggleState();

    juce::Path shape;
    shape.addRoundedRectangle(area, radius);

    juce::ColourGradient fill(juce::Colour(on ? 0xff3a3d43 : (highlighted ? 0xff2a2c31 : 0xff222327)), area.getTopLeft(),
                              juce::Colour(on ? 0xff222428 : 0xff141518), area.getBottomLeft(), false);
    g.setGradientFill(fill);
    g.fillPath(shape);
    if (down) {
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillPath(shape);
    }

    juce::ColourGradient edge(juce::Colours::white.withAlpha(highlighted ? 0.32f : 0.2f), area.getTopLeft(),
                              juce::Colours::white.withAlpha(0.04f), area.getBottomLeft(), false);
    g.setGradientFill(edge);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
}

void MetalLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(!button.isEnabled() ? colour::textFaint : (highlighted || button.getToggleState() ? colour::textHi : colour::text));
    g.drawText(button.getButtonText(), button.getLocalBounds().reduced(8, 0), juce::Justification::centred, false);
}

juce::Font MetalLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return caps(juce::jlimit(10.0f, 12.5f, static_cast<float>(buttonHeight) * 0.38f));
}

void MetalLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    juce::Path shape;
    shape.addRoundedRectangle(area, 6.0f);
    juce::ColourGradient fill(juce::Colour(0xff222327), area.getTopLeft(), juce::Colour(0xff151619), area.getBottomLeft(), false);
    g.setGradientFill(fill);
    g.fillPath(shape);
    juce::ColourGradient edge(juce::Colours::white.withAlpha(box.isMouseOver() ? 0.26f : 0.15f), area.getTopLeft(),
                              juce::Colours::white.withAlpha(0.03f), area.getBottomLeft(), false);
    g.setGradientFill(edge);
    g.strokePath(shape, juce::PathStrokeType(1.0f));

    // Chevron.
    const float cx = area.getRight() - 11.0f, cy = area.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath(cx - 3.5f, cy - 1.5f);
    chevron.lineTo(cx, cy + 2.0f);
    chevron.lineTo(cx + 3.5f, cy - 1.5f);
    g.setColour(colour::textDim);
    g.strokePath(chevron, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font MetalLookAndFeel::getComboBoxFont(juce::ComboBox&) { return sans(13.0f); }

void MetalLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(8, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font MetalLookAndFeel::getPopupMenuFont() { return sans(13.5f); }

void MetalLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
    g.setColour(colour::lineHi);
    g.drawRect(0, 0, width, height, 1);
}

void MetalLookAndFeel::fillTextEditorBackground(juce::Graphics&, int, int, juce::TextEditor&) {}

void MetalLookAndFeel::drawTextEditorOutline(juce::Graphics&, int, int, juce::TextEditor&) {}

void MetalLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                                     int thumbStart, int thumbSize, bool over, bool down)
{
    juce::Rectangle<float> thumb;
    if (vertical)
        thumb = {static_cast<float>(x) + width * 0.5f - 1.5f, static_cast<float>(thumbStart), 3.0f, static_cast<float>(thumbSize)};
    else
        thumb = {static_cast<float>(thumbStart), static_cast<float>(y) + height * 0.5f - 1.5f, static_cast<float>(thumbSize), 3.0f};
    g.setColour(colour::lineHi.brighter(over || down ? 0.4f : 0.0f));
    g.fillRoundedRectangle(thumb, 1.5f);
}

void MetalLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // A pill switch with a metal slider; lit when on.
    const auto area = button.getLocalBounds().toFloat().withSizeKeepingCentre(30.0f, 16.0f);
    const bool on = button.getToggleState();
    juce::Path track;
    track.addRoundedRectangle(area, 8.0f);
    g.setColour(on ? juce::Colour(0xff4a4e55) : juce::Colour(0xff17181b));
    g.fillPath(track);
    g.setColour(juce::Colours::white.withAlpha(highlighted ? 0.25f : 0.14f));
    g.strokePath(track, juce::PathStrokeType(1.0f));

    const float d = 12.0f;
    const auto knob = juce::Rectangle<float>(on ? area.getRight() - d - 2.0f : area.getX() + 2.0f, area.getCentreY() - d * 0.5f, d, d);
    juce::ColourGradient metal(colour::silverHi, knob.getTopLeft(), juce::Colour(0xff6c7178), knob.getBottomRight(), false);
    g.setGradientFill(metal);
    g.fillEllipse(knob);
    if (on) {
        g.setColour(juce::Colours::white.withAlpha(0.18f));
        g.drawEllipse(knob.expanded(2.0f), 1.0f);
    }
}

void MetalLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height)
{
    g.fillAll(findColour(juce::TooltipWindow::backgroundColourId));
    g.setColour(colour::lineHi);
    g.drawRect(0, 0, width, height, 1);
    g.setColour(colour::text);
    g.setFont(sans(12.5f));
    g.drawFittedText(text, 8, 4, width - 16, height - 8, juce::Justification::centredLeft, 4);
}

// ---------------------------------------------------------------- controls

Knob::Knob(juce::RangedAudioParameter& parameter, int paramIndex, const juce::String& label)
    : index_(paramIndex), label_(label.isNotEmpty() ? label : parameter.getName(40))
{
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setRotaryParameters(juce::MathConstants<float>::pi * 1.22f, juce::MathConstants<float>::pi * 2.78f, true);
    const auto& s = synthmatch::spec(paramIndex);
    if (s.min < 0.0f && s.max > 0.0f)
        slider_.getProperties().set("bipolar", true);
    slider_.setPopupDisplayEnabled(false, false, nullptr);
    addAndMakeVisible(slider_);
    attachment_ = std::make_unique<juce::SliderParameterAttachment>(parameter, slider_);
    slider_.onValueChange = [this] { repaint(); };
    slider_.setDoubleClickReturnValue(true, parameter.convertFrom0to1(parameter.getDefaultValue()));
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromBottom(30);
    slider_.setBounds(area);
}

void Knob::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().removeFromBottom(30);
    g.setFont(caps(9.5f));
    g.setColour(colour::textDim);
    g.drawText(label_.toUpperCase(), area.removeFromTop(14), juce::Justification::centred, true);
    g.setFont(sans(12.0f));
    g.setColour(slider_.isMouseOverOrDragging() ? colour::textHi : colour::text);
    const auto text = synthmatch::assist::formatValue(index_, static_cast<float>(slider_.getValue()));
    g.drawText(juce::String::fromUTF8(text.c_str()), area, juce::Justification::centred, true);
}

Choice::Choice(juce::RangedAudioParameter& parameter, int paramIndex, const juce::String& label)
    : label_(label.isNotEmpty() ? label : parameter.getName(40))
{
    const auto& s = synthmatch::spec(paramIndex);
    for (std::size_t i = 0; i < s.choices.size(); ++i)
        box_.addItem(juce::String::fromUTF8(std::string(s.choices[i]).c_str()), static_cast<int>(i) + 1);
    addAndMakeVisible(box_);
    attachment_ = std::make_unique<juce::ComboBoxParameterAttachment>(parameter, box_);
}

void Choice::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(16);
    box_.setBounds(area.withSizeKeepingCentre(area.getWidth(), 26));
}

void Choice::paint(juce::Graphics& g)
{
    g.setFont(caps(9.5f));
    g.setColour(colour::textDim);
    g.drawText(label_.toUpperCase(), getLocalBounds().removeFromTop(14), juce::Justification::centred, true);
}

Switch::Switch(juce::RangedAudioParameter& parameter, const juce::String& label)
    : label_(label.isNotEmpty() ? label : parameter.getName(40))
{
    addAndMakeVisible(button_);
    attachment_ = std::make_unique<juce::ButtonParameterAttachment>(parameter, button_);
}

void Switch::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(16);
    button_.setBounds(area.withSizeKeepingCentre(40, 26));
}

void Switch::paint(juce::Graphics& g)
{
    g.setFont(caps(9.5f));
    g.setColour(colour::textDim);
    g.drawText(label_.toUpperCase(), getLocalBounds().removeFromTop(14), juce::Justification::centred, true);
}

} // namespace theme
