#pragma once

// Offline rendering: parameters + note events -> audio. Used by the render CLI,
// the tests, and later by the matcher and dataset generator.

#include "synthmatch/Params.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace synthmatch {

struct NoteEvent {
    double startSec = 0.0;
    double durationSec = 1.0; // time until note-off
    int note = 60;
    float velocity = 0.8f;
};

struct RenderSettings {
    double sampleRate = 48000.0;
    double tailSec = 1.0; // rendered after the last note-off
    int blockSize = 256;
};

struct StereoBuffer {
    double sampleRate = 48000.0;
    std::vector<float> left;
    std::vector<float> right;

    std::size_t numFrames() const { return left.size(); }
};

// Note events are sample-accurate. The result is deterministic.
StereoBuffer render(const ParamValues& params, std::span<const NoteEvent> notes,
                    const RenderSettings& settings = {});

// Writes a 32-bit float stereo WAV file.
bool writeWav(const std::filesystem::path& path, const StereoBuffer& audio, std::string* error = nullptr);

} // namespace synthmatch
