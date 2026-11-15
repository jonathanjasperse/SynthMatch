#pragma once

// The look: dark gunmetal greys with polished-metal highlights.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace theme {

namespace colour {
inline const juce::Colour bg{0xff0a0b0d};
inline const juce::Colour bgRaised{0xff131417};
inline const juce::Colour panel{0xff16171a};
inline const juce::Colour panelHi{0xff1e1f23};
inline const juce::Colour line{0xff2a2c31};
inline const juce::Colour lineHi{0xff3c3f45};
inline const juce::Colour textHi{0xffeceef1};
inline const juce::Colour text{0xffbfc2c7};
inline const juce::Colour textDim{0xff8a8e95};
inline const juce::Colour textFaint{0xff5a5e65};
inline const juce::Colour silverHi{0xfff3f4f6};
inline const juce::Colour silverLo{0xff8f949c};
inline const juce::Colour warn{0xffc9a896}; // the only warm tone: errors
} // namespace colour

juce::Font sans(float size);
juce::Font light(float size);    // thin numerals and headings
juce::Font medium(float size);
juce::Font caps(float size);     // spaced capitals for labels: pass upper-case text
juce::Font mono(float size);

// A translucent panel that lets the background geometry show through.
void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, float radius = 12.0f, float opacity = 0.78f);

// A polished-metal gradient across an area, for text and strokes.
juce::ColourGradient silver(juce::Rectangle<float> area);

// A small metal glyph used as the plugin's mark.
void drawMark(juce::Graphics& g, juce::Rectangle<float> area);

class MetalLookAndFeel : public juce::LookAndFeel_V4 {
public:
    MetalLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle,
                          float endAngle, juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool highlighted,
                              bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;

    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getDefaultScrollbarWidth() override { return 8; }

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;
};

// A rotary knob bound to one synth parameter, with its name and value below.
class Knob : public juce::Component {
public:
    Knob(juce::RangedAudioParameter& parameter, int paramIndex, const juce::String& label = {});

    void paint(juce::Graphics&) override;
    void resized() override;

    int paramIndex() const { return index_; }

private:
    int index_;
    juce::String label_;
    juce::Slider slider_;
    std::unique_ptr<juce::SliderParameterAttachment> attachment_;
};

// A choice parameter as a drop-down, with its name above.
class Choice : public juce::Component {
public:
    Choice(juce::RangedAudioParameter& parameter, int paramIndex, const juce::String& label = {});
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::String label_;
    juce::ComboBox box_;
    std::unique_ptr<juce::ComboBoxParameterAttachment> attachment_;
};

// An on/off parameter as a small lit switch.
class Switch : public juce::Component {
public:
    Switch(juce::RangedAudioParameter& parameter, const juce::String& label = {});
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::String label_;
    juce::ToggleButton button_;
    std::unique_ptr<juce::ButtonParameterAttachment> attachment_;
};

} // namespace theme
