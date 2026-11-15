#pragma once

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
// Everything that isn't audio (matching, the assistant, undo, presets) lives in
// Session, which this class owns so that it survives the editor being closed.
class SynthMatchProcessor final : public juce::AudioProcessor {
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

    // The reference recording, for listening to it through the plugin.
    void setReference(std::shared_ptr<const synthmatch::StereoBuffer> audio);
    void playReference(double fromSec = 0.0);
    bool isPlayingReference() const { return auditioning_.load(); }

    // Plays notes through the synth, for trying a sound without a keyboard.
    void playNotes(const std::vector<synthmatch::NoteEvent>& notes, double tailSec);
    bool isPlayingNotes() const { return previewing_.load(); }
    void stopPlayback();

    double sampleRate() const { return currentSampleRate_.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleMidi(const juce::MidiMessage& message);
    void takePreviewRequest();
    void renderReference(float* left, float* right, int numSamples);

    juce::AudioProcessorValueTreeState apvts_;
    std::array<juce::RangedAudioParameter*, synthmatch::kNumParams> params_{};
    std::array<std::atomic<float>*, synthmatch::kNumParams> rawParams_{};

    synthmatch::Engine engine_;
    synthmatch::ParamValues engineParams_{}; // what the engine last received (audio thread)
    std::atomic<double> currentSampleRate_{48000.0};

    // Match EQ: written by the interface, picked up by the audio thread.
    std::array<std::atomic<float>, synthmatch::kMatchEqBands> matchEq_{};
    std::atomic<std::uint32_t> matchEqVersion_{0};
    std::uint32_t matchEqSeen_ = 0;

    // The reference recording. The audio thread only ever try-locks, so it never waits.
    juce::SpinLock referenceLock_;
    std::shared_ptr<const synthmatch::StereoBuffer> reference_;
    double referencePos_ = 0.0;
    std::atomic<bool> auditioning_{false};

    // Note preview.
    struct PreviewNote {
        std::int64_t on = 0, off = 0; // samples from the start
        int note = 60;
        float velocity = 0.8f;
    };
    static constexpr int kMaxPreviewNotes = 64;
    juce::SpinLock previewLock_;
    std::array<PreviewNote, kMaxPreviewNotes> pendingNotes_{};
    int pendingCount_ = 0;
    double pendingTail_ = 0.0;
    std::uint32_t pendingRequest_ = 0;
    // Audio thread copy.
    std::array<PreviewNote, kMaxPreviewNotes> previewNotes_{};
    int previewCount_ = 0;
    std::int64_t previewPos_ = 0, previewEnd_ = 0;
    std::uint32_t previewSeen_ = 0;
    std::atomic<bool> previewing_{false};

    std::unique_ptr<Session> session_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthMatchProcessor)
};
