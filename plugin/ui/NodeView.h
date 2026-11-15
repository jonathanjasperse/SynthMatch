#pragma once

// The node view: every part of the synth as a node, wired in signal order.
// Click a node to edit it in the strip below (click empty space for the key
// controls); drag nodes to arrange them; click a node's light to switch it on or off.
//
// The wiring follows the engine's fixed signal path. Nodes can be moved but not
// re-routed yet.

#include "synthmatch/Params.h"
#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <map>
#include <memory>
#include <vector>

class SynthMatchProcessor;
class Session;

namespace ui {

class NodeView : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    NodeView(SynthMatchProcessor& processor, Session& session);
    ~NodeView() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void select(const juce::String& key);

private:
    struct Node;
    class Canvas;
    class Inspector;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void saveLayout();
    void loadLayout();
    static std::vector<Node> makeNodes();

    SynthMatchProcessor& processor_;
    Session& session_;
    std::vector<Node> nodes_;
    std::unique_ptr<Canvas> canvas_;
    std::unique_ptr<Inspector> inspector_;
    juce::String selected_; // empty: the strip shows the key controls
    synthmatch::ParamValues lastParams_{};
};

} // namespace ui
