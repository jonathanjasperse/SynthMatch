#pragma once

// Works out which notes a reference recording plays: pitch tracking (the YIN
// method), then splitting the recording into notes.
//
// This expects one note at a time (a single note, a bass line, a lead). With
// chords the pitch tracker can't lock on; `confidence` drops and the result is
// a best guess.

#include "synthmatch/Render.h"

#include <cstddef>
#include <span>
#include <vector>

namespace synthmatch::match {

struct PitchFrame {
    double timeSec = 0.0;    // centre of the frame
    double hz = 0.0;         // 0 when no pitch was found
    double confidence = 0.0; // 0..1
    double levelDb = -200.0;
};

// One estimate every 10 ms.
std::vector<PitchFrame> trackPitch(std::span<const float> mono, double sampleRate);

struct NoteDetection {
    std::vector<NoteEvent> notes;  // times are relative to startSample
    std::size_t startSample = 0;   // where the analysed section starts in the input
    std::size_t numSamples = 0;    // and how long it is
    double confidence = 0.0;       // 0..1: how clearly pitched the recording is
    // The pitch track the notes were found from, over the analysed section.
    // Times are relative to startSample (frames just before it are negative).
    std::vector<PitchFrame> pitch;
};

// Finds the notes in the first `maxSeconds` of sound (leading silence is
// skipped). Returns no notes if the recording is silent.
NoteDetection detectNotes(std::span<const float> mono, double sampleRate, double maxSeconds = 4.0);

} // namespace synthmatch::match
