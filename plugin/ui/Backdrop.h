#pragma once

// The background: deep gunmetal tiled with small glossy shapes, lit from the
// upper right, drawn once per size into an image.
//
// The tile is either your own image (plugin/assets/tile.png, see the README
// there) or, without one, a drawn metal pentagon.

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme {

// `scale` is the display's pixel density (2 on most laptops' built-in screens).
juce::Image renderBackdrop(int width, int height, float scale);

// One faceted metal pentagon: five bevelled sides around a dark glossy top,
// `radius` from the centre to each corner. `shine` (0..1) is how strongly it
// catches the light.
void drawGem(juce::Graphics& g, juce::Point<float> centre, float radius, float opacity = 1.0f, float shine = 0.5f);

// Your tile image, or an invalid image when there isn't one. It comes from,
// in order: the file named by the SYNTHMATCH_TILE environment variable (read
// again whenever the file changes, for trying designs without rebuilding), or
// plugin/assets/tile.png as built into the plugin.
juce::Image customTile();

// Your tile scaled to fit `maxPixels` square (kept until the tile or the size
// changes), or an invalid image when there isn't one.
juce::Image tileIcon(int maxPixels);

// Changes whenever a file named by SYNTHMATCH_TILE or SYNTHMATCH_BACKDROP is
// saved again; empty when neither is set. The editor watches it to redraw
// the background while you work on a design.
juce::String designFilesStamp();

} // namespace theme
