#pragma once

// The assistant: turns a typed request ("make it brighter", "add more reverb",
// "set the cutoff to 2 kHz") into edits to the current preset, using a small
// language model. This file builds the prompt and reads the model's answer;
// actually running a model is the caller's job (the plugin talks to a local
// model server).

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Edits.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synthmatch::assist {

// A few lines describing a preset in plain words, for /show.
std::string describePreset(const Preset& preset);

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
