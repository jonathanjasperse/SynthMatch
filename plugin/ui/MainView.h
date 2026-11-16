#pragma once

// The main screen: the reference recording and the match. (The knobs live in
// the node view, behind Edit.)

#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

class SynthMatchProcessor;
class Session;

namespace ui {

// A pill button with a small drawn icon.
class IconButton : public juce::Button {
public:
    enum class Icon { None, Play, Stop, Plus, Refresh, Open, Save, Back, Nodes, Info };

    IconButton(const juce::String& text, Icon icon);
    void setIcon(Icon icon) { icon_ = icon; repaint(); }
    void setFramed(bool framed) { framed_ = framed; repaint(); }
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    int idealWidth() const;

    static void drawIcon(juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour);

private:
    Icon icon_;
    bool framed_ = true;
};

class MainView : public juce::Component, private juce::ChangeListener {
public:
    MainView(SynthMatchProcessor& processor, Session& session);
    ~MainView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;

    void setDragHover(bool hover);
    std::function<void()> onChooseFile;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void updateButtons();
    void paintReference(juce::Graphics&, juce::Rectangle<float> area);
    void paintEmpty(juce::Graphics&, juce::Rectangle<float> area);
    void paintScore(juce::Graphics&, juce::Rectangle<float> area);
    void paintSpectrum(juce::Graphics&, juce::Rectangle<float> area);

    SynthMatchProcessor& processor_;
    Session& session_;
    juce::Rectangle<float> leftCard_, rightCard_;
    bool dragHover_ = false;

    IconButton playReference_{"Reference", IconButton::Icon::Play};
    IconButton playSynth_{"Synth", IconButton::Icon::Play};
    IconButton matchAgain_{"Match again", IconButton::Icon::Refresh};
    IconButton stop_{"Stop", IconButton::Icon::Stop};
    IconButton choose_{"Choose a file", IconButton::Icon::Plus};
    IconButton replace_{"Replace", IconButton::Icon::Plus};
};

} // namespace ui
