#pragma once

#include "synthmatch/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

// Thin JUCE wrapper around synthmatch::Engine. Parameters come from the engine's
// schema; this class only translates host parameters and MIDI into engine calls.
class SynthMatchProcessor final : public juce::AudioProcessor {
public:
    SynthMatchProcessor();

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
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& state() { return apvts_; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleMidi(const juce::MidiMessage& message);

    juce::AudioProcessorValueTreeState apvts_;
    std::array<std::atomic<float>*, synthmatch::kNumParams> rawParams_{};
    synthmatch::Engine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthMatchProcessor)
};
