#pragma once

// The background: deep gunmetal tiled with small glossy metal pentagons, lit
// from the upper right, drawn once per size into an image.

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme {

// `scale` is the display's pixel density (2 on most laptops' built-in screens).
juce::Image renderBackdrop(int width, int height, float scale);

// One faceted metal pentagon: five bevelled sides around a dark glossy top,
// `radius` from the centre to each corner. `shine` (0..1) is how strongly it
// catches the light.
void drawGem(juce::Graphics& g, juce::Point<float> centre, float radius, float opacity = 1.0f, float shine = 0.5f);

} // namespace theme
