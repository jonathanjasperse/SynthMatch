#pragma once

// The assistant: turns a typed request ("make it brighter", "add more reverb",
// "set the cutoff to 2 kHz") into edits to the current preset.
//
// Two interpreters produce the same kind of answer:
//
//  - interpret(): built-in rules. Instant, always available, and exact for
//    requests that name a parameter. It also knows a vocabulary of everyday
//    words (brighter, warmer, wider, punchier, plucky, ...).
//
//  - a small language model, for requests the rules don't cover. This file
//    builds the prompt and reads the model's answer; actually running a model
//    is the caller's job (the plugin talks to a local model server).

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Edits.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synthmatch::assist {

struct Reply {
    enum class Action {
        None,  // not understood
        Edit,  // apply `edits`
        Undo,
        Redo,
        Reset, // back to the initial preset
        Help,
        Show   // describe the current preset
    };

    Action action = Action::None;
    std::vector<Edit> edits;
    std::string text; // something to say; for edits the caller adds the list of changes
    // True when the request named its parameters and values outright ("cutoff
    // 2 kHz", "undo"), so there was nothing to interpret. Vaguer requests
    // ("warmer") are better handed to a language model when one is available.
    bool exact = false;
    // True when every word of the request was one the rules know. When false,
    // part of the request may have been ignored, so a language model should
    // have a go at it.
    bool complete = false;
};

// The built-in rules.
Reply interpret(std::string_view request, const ParamValues& current);

std::string helpText();

// A few lines describing a preset in plain words, for "show".
std::string describePreset(const Preset& preset);

// ---- Language model support ----

// What to send to a chat model: instructions and examples, then the request
// together with the current settings.
std::string modelSystemPrompt();
std::string modelUserPrompt(std::string_view request, const ParamValues& current);

// A JSON Schema for the answer. Servers that support structured output use it
// to make the model produce valid edits and nothing else.
Json modelReplySchema();

// Reads a model's answer. Text around the JSON is ignored. Returns nothing if
// no usable edits were found.
struct ModelReply {
    std::vector<Edit> edits;
    std::string text;
};
std::optional<ModelReply> parseModelReply(std::string_view answer, std::string* error = nullptr);

// Holds the model to what the request plainly asked for, where small models
// slip: "vibrato" always means LFO 1 on pitch (a smooth wave, about 5.5 Hz,
// a vibrato-sized depth unless the model set its own).
void reinforceModelReply(std::string_view request, ModelReply& reply);

} // namespace synthmatch::assist
