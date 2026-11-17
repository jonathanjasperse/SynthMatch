#pragma once

#include "RealtimeTools.h"
#include "synthmatch/Engine.h"
#include "synthmatch/Render.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class Session;

// Thin JUCE wrapper around synthmatch::Engine. Parameters come from the engine's
// schema; this class translates host parameters and MIDI into engine calls, and
// hands data from the interface to the audio thread without locking it.
//
// The audio thread never locks (not even a try-lock), allocates or frees. Data
// from the interface arrives through atomics and a wait-free triple buffer, and
// memory it used is released on the message thread. JUCE's own wrapper still
// takes its callback lock around processBlock and a parameter's listener lock
// when the host automates it; plugin code can't remove those, but nothing here
// ever holds them for long (no plugin code listens to parameters).
//
// Everything that isn't audio (matching, the assistant, undo, presets) lives in
// Session, which this class owns so that it survives the editor being closed.
class SynthMatchProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater {
public:
    SynthMatchProcessor();
    ~SynthMatchProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- Message thread ----

    juce::AudioProcessorValueTreeState& state() { return apvts_; }
    Session& session() { return *session_; }

    juce::RangedAudioParameter& parameter(int index) { return *params_[static_cast<std::size_t>(index)]; }
    synthmatch::ParamValues currentParams() const;
    // Sets parameters the way a person turning knobs would, so the host records it.
    void setParams(const synthmatch::ParamValues& values);

    void setMatchEq(const synthmatch::MatchEqGains& gainsDb);
    synthmatch::MatchEqGains matchEq() const;

    // The reference recording, for listening to it through the plugin. It is
    // converted to the device rate here, on the message thread, so the audio
    // thread only copies samples.
    void setReference(std::shared_ptr<const synthmatch::StereoBuffer> audio);
    // Each of these replaces whatever was playing.
    void playReference(double fromSec = 0.0);
    bool isPlayingReference() const { return auditioning_.load(); }

    // Plays notes through the synth, for trying a sound without a keyboard.
    void playNotes(const std::vector<synthmatch::NoteEvent>& notes, double tailSec);
    bool isPlayingNotes() const { return previewing_.load(); }
    void stopPlayback();

    double sampleRate() const { return currentSampleRate_.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleMidi(const juce::uint8* data, int numBytes);
    void takePlayRequest();
    void renderReference(float* left, float* right, int numSamples);
    void stopReference();
    // Message thread, after prepareToPlay: converts the reference to a new device rate.
    void handleAsyncUpdate() override;

    juce::AudioProcessorValueTreeState apvts_;
    std::array<juce::RangedAudioParameter*, synthmatch::kNumParams> params_{};
    std::array<std::atomic<float>*, synthmatch::kNumParams> rawParams_{};

    synthmatch::Engine engine_;
    synthmatch::ParamValues engineParams_{}; // what the engine last received (audio thread)
    std::atomic<double> currentSampleRate_{48000.0};
    double blockRate_ = 48000.0; // the rate prepareToPlay gave (audio thread)
    rt::SafetyLimiter limiter_;  // audio thread

    // Match EQ: written by the interface, picked up by the audio thread. The
    // version is odd while a write is under way, so the audio thread never
    // takes a half-written set.
    std::array<std::atomic<float>, synthmatch::kMatchEqBands> matchEq_{};
    std::atomic<std::uint32_t> matchEqVersion_{0};
    std::uint32_t matchEqSeen_ = 0;

    // Playback requests from the interface (message thread) to the audio thread.
    // Each one says everything that should play: preview notes, the reference
    // from a position, or nothing (stop). The newest one wins.
    struct PreviewNote {
        std::int64_t on = 0, off = 0; // samples from the start
        int note = 60;
        float velocity = 0.8f;
    };
    static constexpr int kMaxPreviewNotes = 64;
    struct PlayRequest {
        std::array<PreviewNote, kMaxPreviewNotes> notes{};
        int count = 0;
        double tailSec = 0.0;
        // The reference at the device rate. Only the message thread copies or
        // releases it; the audio thread only reads through it.
        std::shared_ptr<const synthmatch::StereoBuffer> reference;
        std::size_t referenceStart = 0;    // frames
        std::uint32_t referenceGeneration = 0; // which setReference it belongs to
    };
    rt::TripleBuffer<PlayRequest> requests_;
    PlayRequest& newRequest(); // message thread: fill it in, then sendRequest()
    void sendRequest();

    // The reference: the file's audio and a copy at the device rate (message
    // thread). A new file bumps the generation, which stops it playing.
    std::shared_ptr<const synthmatch::StereoBuffer> referenceSource_, referencePlayback_;
    std::atomic<std::uint32_t> referenceGeneration_{0};

    // Audio thread state for the request it is playing.
    std::int64_t previewPos_ = 0, previewEnd_ = 0;
    bool previewActive_ = false, referenceActive_ = false;
    std::size_t referencePos_ = 0;
    // For the interface to show. The message thread sets them just before it
    // sends a request; the audio thread corrects them when it takes one or
    // finishes playing.
    std::atomic<bool> auditioning_{false}, previewing_{false};

    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::unique_ptr<Session> session_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthMatchProcessor)
};
