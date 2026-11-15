#pragma once

// Talks to a small language model running on this computer.
//
// The plugin doesn't run the model itself: a local model server does (Ollama
// by default; LM Studio, llama.cpp's server and others with an OpenAI-style
// API also work). That keeps the plugin small, lets the person choose a model
// that suits their machine, and keeps a heavy runtime out of the audio host.
// Nothing is sent anywhere except the server address in the settings, which
// is this computer by default.
//
// All functions here block, so call them from a background thread.

#include "synthmatch/Json.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace llm {

struct Settings {
    juce::String server = "http://127.0.0.1:11434";
    juce::String model = "qwen3:1.7b";
    // When to ask the model: "auto" for requests the built-in rules don't fully
    // understand, "always" for every request that isn't an exact setting, "off".
    juce::String mode = "auto";
    bool enabled() const { return mode != "off"; }
};

struct ModelInfo {
    juce::String name;
    double bytes = 0.0;    // size on disk, when the server says
    bool thinking = false; // it can "think" before answering (which we switch off: it's slow)
};

struct Probe {
    bool reachable = false;  // something answered at the address
    bool ollama = false;     // it speaks Ollama's API (otherwise the OpenAI-style one)
    bool hasModel = false;   // the chosen model is installed
    std::vector<ModelInfo> models;
    juce::String error;

    juce::StringArray names() const;
    const ModelInfo* find(const juce::String& model) const;
    // The smallest installed model under `maxBytes`, or empty.
    juce::String smallest(double maxBytes) const;
};

Probe probe(const Settings& settings);

struct Answer {
    bool ok = false;
    juce::String text;  // the model's reply
    juce::String error;
};

// Sends one request. `schema` (a JSON Schema) asks the server to constrain the
// answer to that shape, when it supports it. `onProgress` is called with the
// number of characters received so far. Returns early when `cancel` is set.
Answer chat(const Settings& settings, bool ollama, bool noThinking, const std::string& system, const std::string& user,
            const synthmatch::Json& schema, const std::atomic<bool>& cancel,
            const std::function<void(int)>& onProgress = {});

// Asks an Ollama server to download a model. `onProgress` gets the bytes done
// and the total (0 while unknown). Returns an empty string on success, or why not.
juce::String pull(const Settings& settings, const juce::String& model, const std::atomic<bool>& cancel,
                  const std::function<void(double done, double total)>& onProgress);

// Where Ollama is normally installed, or a nonexistent file.
juce::File installedApp();

} // namespace llm
