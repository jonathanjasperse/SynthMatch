#include "ui/Backdrop.h"

#include <array>
#include <cmath>

namespace theme {

namespace {

using Pt = juce::Point<float>;

constexpr float kPi = juce::MathConstants<float>::pi;
// Light comes from the upper left of each gem.
constexpr float kLightAngle = -2.2f;

juce::Path polygon(std::initializer_list<Pt> points)
{
    juce::Path p;
    bool first = true;
    for (const auto& q : points) {
        if (first)
            p.startNewSubPath(q);
        else
            p.lineTo(q);
        first = false;
    }
    p.closeSubPath();
    return p;
}

juce::Colour metal(float shade)
{
    // From near-black gunmetal to bright polished steel.
    shade = juce::jlimit(0.0f, 1.0f, shade);
    return juce::Colour(0xff0b0c0e).interpolatedWith(juce::Colour(0xffa7acb4), std::pow(shade, 1.6f));
}

// A soft band of light across a shape, like a reflection on polished metal.
void streak(juce::Graphics& g, const juce::Path& clip, Pt from, Pt to, float strength)
{
    if (strength <= 0.0f)
        return;
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(clip);
    juce::ColourGradient band(juce::Colours::white.withAlpha(0.0f), from, juce::Colours::white.withAlpha(0.0f), to, false);
    band.addColour(0.40, juce::Colours::white.withAlpha(0.0f));
    band.addColour(0.5, juce::Colours::white.withAlpha(strength));
    band.addColour(0.60, juce::Colours::white.withAlpha(0.0f));
    g.setGradientFill(band);
    g.fillPath(clip);
}

} // namespace

void drawGem(juce::Graphics& g, Pt c, float radius, float opacity, float shine)
{
    juce::Graphics::ScopedSaveState save(g);
    g.setOpacity(opacity);

    // Corners of the outline and of the flat top, pointing up.
    std::array<Pt, 5> outer, inner;
    const float top = radius * 0.5f;
    for (int i = 0; i < 5; ++i) {
        const float a = -kPi * 0.5f + static_cast<float>(i) * 2.0f * kPi / 5.0f;
        outer[static_cast<std::size_t>(i)] = c + Pt(std::cos(a), std::sin(a)) * radius;
        // The top sits a touch up and to the left: the gem is seen slightly from above.
        inner[static_cast<std::size_t>(i)] = c + Pt(-0.04f, -0.05f) * radius + Pt(std::cos(a), std::sin(a)) * top;
    }
    const auto outline = polygon({outer[0], outer[1], outer[2], outer[3], outer[4]});
    const auto table = polygon({inner[0], inner[1], inner[2], inner[3], inner[4]});

    juce::DropShadow(juce::Colours::black.withAlpha(0.75f), juce::roundToInt(radius * 0.22f), {0, juce::roundToInt(radius * 0.08f)})
        .drawForPath(g, outline);

    // The five bevelled sides, each lit by how much it faces the light.
    for (int i = 0; i < 5; ++i) {
        const auto j = static_cast<std::size_t>((i + 1) % 5);
        const auto k = static_cast<std::size_t>(i);
        const auto facet = polygon({outer[k], outer[j], inner[j], inner[k]});
        const float facing = -kPi * 0.5f + (static_cast<float>(i) + 0.5f) * 2.0f * kPi / 5.0f;
        const float shade = 0.5f + 0.5f * std::cos(facing - kLightAngle);
        const Pt edgeMid = (outer[k] + outer[j]) * 0.5f, innerMid = (inner[k] + inner[j]) * 0.5f;
        g.setGradientFill(juce::ColourGradient(metal(shade * 0.75f), edgeMid, metal(shade * (0.85f + 0.15f * shine)), innerMid, false));
        g.fillPath(facet);

        // A faint oil-on-metal shimmer: cool on the left, warm on the right.
        const bool left = std::cos(facing) < 0.0f;
        g.setColour(left ? juce::Colour(0x149db0ff) : juce::Colour(0x0cffc8a6));
        g.fillPath(facet);

        streak(g, facet, outer[k], inner[j], 0.10f * shade * (0.4f + shine));
    }

    // The top: dark, glossy, with a reflection.
    const auto& b = table.getBounds();
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2a2c31), b.getTopLeft(), juce::Colour(0xff050506), b.getBottomRight(), false));
    g.fillPath(table);
    {
        juce::Graphics::ScopedSaveState inner2(g);
        g.reduceClipRegion(table);
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.10f + 0.22f * shine), b.getTopLeft(),
                                               juce::Colours::transparentWhite, b.getCentre(), true));
        g.fillPath(table);
    }
    streak(g, table, {b.getX(), b.getY()}, {b.getRight(), b.getBottom()}, 0.05f + 0.12f * shine);

    // Edges: bright where they face the light, nearly gone elsewhere.
    for (int i = 0; i < 5; ++i) {
        const auto j = static_cast<std::size_t>((i + 1) % 5);
        const auto k = static_cast<std::size_t>(i);
        const float facing = -kPi * 0.5f + (static_cast<float>(i) + 0.5f) * 2.0f * kPi / 5.0f;
        const float lit = 0.5f + 0.5f * std::cos(facing - kLightAngle);
        g.setColour(juce::Colours::white.withAlpha(0.04f + 0.30f * lit * (0.5f + 0.5f * shine)));
        g.drawLine({outer[k], outer[j]}, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.06f + 0.40f * (1.0f - lit) * (0.5f + 0.5f * shine)));
        g.drawLine({inner[k], inner[j]}, 1.0f); // the top's rim catches the light from the other side
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawLine({outer[k], inner[k]}, 0.8f);
    }
}

juce::Image renderBackdrop(int width, int height, float scale)
{
    const int pw = std::max(1, juce::roundToInt(static_cast<float>(width) * scale));
    const int ph = std::max(1, juce::roundToInt(static_cast<float>(height) * scale));
    juce::Image image(juce::Image::ARGB, pw, ph, true);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));

    const auto w = static_cast<float>(width), h = static_cast<float>(height);

    // Base: deep gunmetal.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff121316), {0.0f, 0.0f}, juce::Colour(0xff060709), {0.0f, h}, false));
    g.fillAll();

    // One gem drawn twice into sprites, dull and shining; every tile is a blend
    // of the two, depending on how close it is to the light.
    const float radius = juce::jlimit(26.0f, 54.0f, h * 0.055f);
    const float pitchX = radius * 2.45f, pitchY = radius * 2.15f;
    const int sprite = juce::roundToInt(radius * 3.2f * scale);
    const auto makeSprite = [&](float shine) {
        juce::Image img(juce::Image::ARGB, sprite, sprite, true);
        juce::Graphics sg(img);
        sg.addTransform(juce::AffineTransform::scale(scale));
        const float s = static_cast<float>(sprite) / scale;
        drawGem(sg, {s * 0.5f, s * 0.5f}, radius, 1.0f, shine);
        return img;
    };
    const auto dull = makeSprite(0.1f);
    const auto bright = makeSprite(1.0f);
    const float spriteSize = static_cast<float>(sprite) / scale;

    const Pt light{w * 0.82f, h * 0.12f};
    const float reach = std::max(w, h) * 0.75f;
    int row = 0;
    for (float y = -pitchY * 0.5f; y < h + pitchY; y += pitchY, ++row) {
        const float offset = (row % 2 == 0) ? 0.0f : pitchX * 0.5f;
        for (float x = -pitchX * 0.5f + offset; x < w + pitchX; x += pitchX) {
            const float d = Pt(x, y).getDistanceFrom(light) / reach;
            const float shine = std::exp(-d * d * 2.2f);
            const auto area = juce::Rectangle<float>(spriteSize, spriteSize).withCentre({x, y});
            g.setOpacity(0.3f + 0.25f * shine);
            g.drawImage(dull, area);
            g.setOpacity(0.6f * shine);
            g.drawImage(bright, area);
        }
    }
    g.setOpacity(1.0f);

    // The light itself: a soft glow over the brightest tiles.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0x22b4bccc), light, juce::Colour(0x00000000), light.translated(reach, reach * 0.5f),
                                           true));
    g.fillAll();

    // Vignette, so the edges fall away into darkness.
    g.setGradientFill(juce::ColourGradient(juce::Colours::transparentBlack, {w * 0.55f, h * 0.42f},
                                           juce::Colours::black.withAlpha(0.7f), {0.0f, h}, true));
    g.fillAll();

    // Fine grain, so the gradients don't band.
    {
        juce::Image::BitmapData data(image, juce::Image::BitmapData::readWrite);
        juce::Random random(7);
        const auto clamp = [](int v) { return static_cast<juce::uint8>(juce::jlimit(0, 255, v)); };
        for (int y = 0; y < ph; ++y) {
            auto* line = data.getLinePointer(y);
            for (int x = 0; x < pw; ++x) {
                auto* px = reinterpret_cast<juce::PixelARGB*>(line + x * data.pixelStride);
                const int n = random.nextInt(5) - 2;
                px->setARGB(px->getAlpha(), clamp(px->getRed() + n), clamp(px->getGreen() + n), clamp(px->getBlue() + n));
            }
        }
    }
    return image;
}

} // namespace theme
