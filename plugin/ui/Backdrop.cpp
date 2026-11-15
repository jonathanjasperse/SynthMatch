#include "ui/Backdrop.h"

#include <cmath>

namespace theme {

namespace {

using Pt = juce::Point<float>;

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

// A soft band of light across a face, like a reflection on polished metal.
void streak(juce::Graphics& g, const juce::Path& clip, Pt from, Pt to, float strength)
{
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(clip);
    juce::ColourGradient band(juce::Colours::white.withAlpha(0.0f), from, juce::Colours::white.withAlpha(0.0f), to, false);
    band.addColour(0.42, juce::Colours::white.withAlpha(0.0f));
    band.addColour(0.5, juce::Colours::white.withAlpha(strength));
    band.addColour(0.58, juce::Colours::white.withAlpha(0.0f));
    g.setGradientFill(band);
    g.fillPath(clip);
}

void tint(juce::Graphics& g, const juce::Path& face, Pt from, juce::Colour a, Pt to, juce::Colour b)
{
    g.setGradientFill(juce::ColourGradient(a, from, b, to, false));
    g.fillPath(face);
}

} // namespace

void drawCube(juce::Graphics& g, Pt c, float r, float cut, float opacity, float rotation)
{
    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::rotation(rotation, c.x, c.y));
    g.setOpacity(opacity);

    // A cube seen corner-on: a hexagon whose centre is the corner nearest to us.
    const float k = std::sqrt(3.0f) * 0.5f;
    const Pt top{c.x, c.y - r}, ur{c.x + k * r, c.y - 0.5f * r}, lr{c.x + k * r, c.y + 0.5f * r};
    const Pt bottom{c.x, c.y + r}, ll{c.x - k * r, c.y + 0.5f * r}, ul{c.x - k * r, c.y - 0.5f * r};
    // That corner is cut off, leaving a small triangular facet.
    const Pt a = c + (ul - c) * cut, b = c + (ur - c) * cut, d = c + (bottom - c) * cut;

    const auto topFace = polygon({top, ur, b, a, ul});
    const auto leftFace = polygon({ul, a, d, bottom, ll});
    const auto rightFace = polygon({ur, lr, bottom, d, b});
    const auto facet = polygon({a, b, d});
    const auto outline = polygon({top, ur, lr, bottom, ll, ul});

    juce::DropShadow(juce::Colours::black.withAlpha(0.8f), static_cast<int>(r * 0.18f), {0, static_cast<int>(r * 0.06f)})
        .drawForPath(g, outline);

    // Faces: the top catches the light, the right side is in shadow.
    tint(g, topFace, top, juce::Colour(0xff6a6e76), (a + b) * 0.5f, juce::Colour(0xff202226));
    tint(g, leftFace, ul, juce::Colour(0xff34363c), bottom, juce::Colour(0xff0b0c0e));
    tint(g, rightFace, ur, juce::Colour(0xff1e2024), bottom, juce::Colour(0xff060607));

    // A faint oil-on-metal shimmer: cool on the left, warm on the right.
    tint(g, topFace, ul, juce::Colour(0x1a9fb4ff), ur, juce::Colour(0x10ffc6a0));
    tint(g, leftFace, ul, juce::Colour(0x149aa8ff), bottom, juce::Colour(0x00000000));
    tint(g, rightFace, lr, juce::Colour(0x0cffd2b4), b, juce::Colour(0x00000000));

    // Reflections.
    streak(g, leftFace, {ll.x - r * 0.1f, ul.y - r * 0.2f}, {d.x + r * 0.2f, bottom.y}, 0.10f);
    streak(g, topFace, {ul.x, top.y}, {ur.x, b.y + r * 0.2f}, 0.14f);
    streak(g, rightFace, {b.x, ur.y}, {lr.x, bottom.y + r * 0.3f}, 0.05f);

    // The facet: dark, glossy glass with a bright upper edge.
    tint(g, facet, (a + b) * 0.5f, juce::Colour(0xff2b2d33), d, juce::Colour(0xff030304));
    {
        juce::Graphics::ScopedSaveState inner(g);
        g.reduceClipRegion(facet);
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.22f), a, juce::Colours::transparentWhite,
                                               (a + d) * 0.5f, true));
        g.fillPath(facet);
    }

    // Edges: polished highlights where the light hits, nearly invisible elsewhere.
    const auto edge = [&](Pt p, Pt q, float alpha, float width = 1.0f) {
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(alpha), p,
                                               juce::Colours::white.withAlpha(alpha * 0.25f), q, false));
        g.drawLine({p, q}, width);
    };
    edge(top, ul, 0.30f);
    edge(top, ur, 0.22f);
    edge(a, b, 0.55f, 1.2f);
    edge(a, ul, 0.20f);
    edge(b, ur, 0.16f);
    edge(a, d, 0.18f);
    edge(b, d, 0.10f);
    edge(d, bottom, 0.12f);
    edge(ul, ll, 0.10f);
    edge(ur, lr, 0.06f);
    edge(ll, bottom, 0.05f);
    edge(lr, bottom, 0.04f);
}

juce::Image renderBackdrop(int width, int height, float scale)
{
    const int pw = std::max(1, juce::roundToInt(static_cast<float>(width) * scale));
    const int ph = std::max(1, juce::roundToInt(static_cast<float>(height) * scale));
    juce::Image image(juce::Image::ARGB, pw, ph, true);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));

    const auto w = static_cast<float>(width), h = static_cast<float>(height);

    // Base: deep gunmetal, a little lighter where the light comes from.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff15161a), {0.0f, 0.0f}, juce::Colour(0xff07080a), {0.0f, h}, false));
    g.fillAll();

    const Pt hero{w * 0.80f, h * 0.40f};
    const float heroR = h * 0.50f;
    g.setGradientFill(juce::ColourGradient(juce::Colour(0x30a9b2c4), hero.translated(-heroR * 0.3f, -heroR * 0.5f),
                                           juce::Colour(0x00000000), hero.translated(heroR * 1.2f, heroR * 1.2f), true));
    g.fillAll();

    // Faint hexagonal rings around the large shape.
    for (const float ring : {1.28f, 1.62f, 2.05f}) {
        juce::Path hex;
        for (int i = 0; i < 6; ++i) {
            const float angle = juce::MathConstants<float>::pi / 3.0f * static_cast<float>(i) - juce::MathConstants<float>::halfPi;
            const Pt p{hero.x + heroR * ring * std::cos(angle), hero.y + heroR * ring * std::sin(angle)};
            if (i == 0)
                hex.startNewSubPath(p);
            else
                hex.lineTo(p);
        }
        hex.closeSubPath();
        g.setColour(juce::Colours::white.withAlpha(0.028f / ring));
        g.strokePath(hex, juce::PathStrokeType(1.0f));
    }

    drawCube(g, {w * 0.09f, h * 0.90f}, h * 0.19f, 0.30f, 0.55f, -0.18f);
    drawCube(g, {w * 0.47f, h * 0.06f}, h * 0.05f, 0.32f, 0.45f, 0.22f);
    drawCube(g, hero, heroR, 0.30f, 0.92f);

    // Vignette.
    g.setGradientFill(juce::ColourGradient(juce::Colours::transparentBlack, {w * 0.5f, h * 0.45f},
                                           juce::Colours::black.withAlpha(0.55f), {0.0f, 0.0f}, true));
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
