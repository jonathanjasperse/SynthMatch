#pragma once

// The About panel: version, licence, credits and where to get the source code.
// The AGPLv3 asks interactive programs to show this kind of notice.

#include "ui/MainView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace ui {

inline constexpr const char* kSourceUrl = "https://github.com/jonathanjasperse/SynthMatch";
inline constexpr const char* kLicenceUrl = "https://www.gnu.org/licenses/agpl-3.0.html";

class About : public juce::Component {
public:
    About();

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    juce::Rectangle<int> panel() const;

    IconButton source_{"Source code", IconButton::Icon::Open};
    IconButton licence_{"Licence", IconButton::Icon::None};
    IconButton close_{"Close", IconButton::Icon::None};
};

} // namespace ui
