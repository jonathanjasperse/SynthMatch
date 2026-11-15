#pragma once

// The background: deep gunmetal with large, glossy metal polyhedra, drawn once
// per size into an image.

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme {

// `scale` is the display's pixel density (2 on most laptops' built-in screens).
juce::Image renderBackdrop(int width, int height, float scale);

// One glossy cube with its front corner cut off, centred on `centre`, `radius`
// from the centre to each outer corner. Exposed for the empty-state artwork.
void drawCube(juce::Graphics& g, juce::Point<float> centre, float radius, float cut, float opacity,
              float rotation = 0.0f);

} // namespace theme
