#include "synthmatch/Render.h"

#include "synthmatch/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace synthmatch {

namespace {

struct TimedEvent {
    std::int64_t sample;
    bool isNoteOn;
    int note;
    float velocity;
};

// std::llround returns long long, which is not the same type as std::int64_t
// on Linux, so convert once here.
std::int64_t toSamples(double seconds, double sampleRate)
{
    return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * sampleRate));
}

} // namespace

namespace {

StereoBuffer renderImpl(const ParamValues& params, std::span<const float> matchEq,
                        std::span<const NoteEvent> notes, const RenderSettings& settings)
{
    const double sampleRate = settings.sampleRate > 0.0 ? settings.sampleRate : 48000.0;
    const int blockSize = std::max(1, settings.blockSize);

    Engine engine;
    engine.prepare(sampleRate);
    engine.setOversampling(settings.oversample);
    engine.setTempo(settings.tempoBpm);
    engine.setParams(params);
    engine.setMatchEq(matchEq);

    std::vector<TimedEvent> events;
    events.reserve(notes.size() * 2);
    std::int64_t lastOff = 0;

    for (const auto& n : notes) {
        const auto on = toSamples(n.startSec, sampleRate);
        // At least one sample long so a note-off never lands on its own note-on.
        const auto off = std::max(on + 1, toSamples(n.startSec + n.durationSec, sampleRate));
        events.push_back({on, true, n.note, n.velocity});
        events.push_back({off, false, n.note, 0.0f});
        lastOff = std::max(lastOff, off);
    }

    // Note-offs before note-ons at the same sample, so a repeated note retriggers.
    std::stable_sort(events.begin(), events.end(), [](const TimedEvent& a, const TimedEvent& b) {
        if (a.sample != b.sample)
            return a.sample < b.sample;
        return !a.isNoteOn && b.isNoteOn;
    });

    const auto total = lastOff + toSamples(settings.tailSec, sampleRate);

    StereoBuffer out;
    out.sampleRate = sampleRate;
    out.left.assign(static_cast<std::size_t>(total), 0.0f);
    out.right.assign(static_cast<std::size_t>(total), 0.0f);

    std::int64_t pos = 0;
    std::size_t next = 0;

    while (pos < total) {
        while (next < events.size() && events[next].sample <= pos) {
            const auto& e = events[next++];
            if (e.isNoteOn)
                engine.noteOn(e.note, e.velocity);
            else
                engine.noteOff(e.note);
        }

        auto end = std::min<std::int64_t>(total, pos + blockSize);
        if (next < events.size())
            end = std::min(end, events[next].sample);

        engine.process(out.left.data() + pos, out.right.data() + pos, static_cast<int>(end - pos));
        pos = end;
    }

    return out;
}

} // namespace

StereoBuffer render(const ParamValues& params, std::span<const NoteEvent> notes, const RenderSettings& settings)
{
    return renderImpl(params, {}, notes, settings);
}

StereoBuffer render(const Preset& preset, std::span<const NoteEvent> notes, const RenderSettings& settings)
{
    return renderImpl(preset.params, preset.matchEq, notes, settings);
}

} // namespace synthmatch
