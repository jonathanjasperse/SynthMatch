#pragma once

// The assistant terminal along the bottom of the window: type a request
// ("make it brighter", "add more reverb"), see what changed.

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

class Session;

namespace ui {

class Terminal : public juce::Component, private juce::ChangeListener, private juce::Timer {
public:
    explicit Terminal(Session& session);
    ~Terminal() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void focusInput();

private:
    class Log;
    class Input;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();
    void submit();

    Session& session_;
    juce::Viewport viewport_;
    std::unique_ptr<Log> log_;
    std::unique_ptr<Input> input_;
    std::vector<juce::String> history_;
    int historyPos_ = 0;
    std::size_t shownEntries_ = 0;
    bool wasBusy_ = false;
    int dots_ = 0;
};

} // namespace ui
