#include "ui/About.h"

#include "ui/Theme.h"

namespace ui {

About::About()
{
    setWantsKeyboardFocus(true);
    for (auto* b : {&source_, &licence_, &close_})
        addAndMakeVisible(*b);
    source_.onClick = [] { juce::URL(kSourceUrl).launchInDefaultBrowser(); };
    licence_.onClick = [] { juce::URL(kLicenceUrl).launchInDefaultBrowser(); };
    close_.onClick = [this] { setVisible(false); };
}

juce::Rectangle<int> About::panel() const { return getLocalBounds().withSizeKeepingCentre(560, 392); }

void About::resized()
{
    auto buttons = panel().reduced(30, 26).removeFromBottom(36);
    close_.setBounds(buttons.removeFromRight(close_.idealWidth()));
    buttons.removeFromRight(10);
    licence_.setBounds(buttons.removeFromRight(licence_.idealWidth()));
    source_.setBounds(buttons.removeFromLeft(source_.idealWidth()));
}

void About::mouseUp(const juce::MouseEvent& e)
{
    if (!panel().contains(e.getPosition()))
        setVisible(false); // a click outside closes it
}

bool About::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) {
        setVisible(false);
        return true;
    }
    return false;
}

void About::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.6f));
    const auto box = panel().toFloat();
    theme::drawPanel(g, box, 16.0f, 0.97f);

    auto area = panel().reduced(30, 26);
    area.removeFromBottom(46);

    theme::drawMark(g, area.removeFromTop(30).removeFromLeft(42).toFloat().withSizeKeepingCentre(36.0f, 22.0f));
    area.removeFromTop(4);
    g.setFont(theme::light(26.0f));
    g.setGradientFill(theme::silver(area.toFloat().removeFromTop(34.0f)));
    g.drawText("SynthMatch " JucePlugin_VersionString, area.removeFromTop(36), juce::Justification::centredLeft, false);

    const auto paragraph = [&](const juce::String& text, juce::Font font, juce::Colour colour, int gapAfter) {
        juce::AttributedString s;
        s.setWordWrap(juce::AttributedString::byWord);
        s.setLineSpacing(2.0f);
        s.append(text, font, colour);
        juce::TextLayout layout;
        layout.createLayout(s, static_cast<float>(area.getWidth()));
        layout.draw(g, area.removeFromTop(static_cast<int>(std::ceil(layout.getHeight()))).toFloat());
        area.removeFromTop(gapAfter);
    };

    paragraph("Listens to a recording of a synth and builds a preset that sounds like it.", theme::sans(13.5f),
              theme::colour::text, 14);
    paragraph(juce::String::fromUTF8("Copyright \xc2\xa9 2026 Bedrot Audio. SynthMatch is free software: you can "
                                     "redistribute it and change it under the terms of the GNU Affero General Public "
                                     "License, version 3 or later. It comes with ABSOLUTELY NO WARRANTY. The source code "
                                     "is at github.com/BedrotAudio/ml-synthmatch."),
              theme::sans(12.5f), theme::colour::textDim, 14);
    paragraph("Built with JUCE. It includes zlib, HarfBuzz, SheenBidi, LunaSVG, PlutoVG, libpng, the Independent JPEG "
              "Group's library, libwebp, FLAC, Ogg, Vorbis, Opus, the VST3 SDK and Apple's Audio Unit SDK. Their "
              "licences are in THIRD_PARTY_NOTICES.txt, installed with SynthMatch and included in the source code.",
              theme::sans(12.5f), theme::colour::textDim, 14);
    paragraph("The AI assistant can use Ollama (MIT licence) and the Qwen3 language model (Apache License 2.0), which "
              "are installed separately and run on this computer.",
              theme::sans(12.5f), theme::colour::textDim, 0);
}

} // namespace ui
