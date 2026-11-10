#include "PluginProcessor.h"

#include "synthmatch/Params.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace {

juce::String toJuce(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

} // namespace

SynthMatchProcessor::SynthMatchProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "SynthMatch", createLayout())
{
    for (int i = 0; i < synthmatch::kNumParams; ++i) {
        rawParams_[static_cast<std::size_t>(i)] = apvts_.getRawParameterValue(toJuce(synthmatch::spec(i).id));
        jassert(rawParams_[static_cast<std::size_t>(i)] != nullptr);
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout SynthMatchProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < synthmatch::kNumParams; ++i) {
        const auto& s = synthmatch::spec(i);

        // The host-facing 0..1 mapping comes from the schema, so the plugin, presets
        // and ML normalization all agree on what a knob position means.
        juce::NormalisableRange<float> range(
            s.min, s.max,
            [i](float, float, float t) { return synthmatch::fromNormalized(synthmatch::spec(i), t); },
            [i](float, float, float v) { return synthmatch::toNormalized(synthmatch::spec(i), v); },
            [i](float, float, float v) { return synthmatch::clampToRange(synthmatch::spec(i), v); });

        auto attributes = juce::AudioParameterFloatAttributes().withLabel(toJuce(s.unit));

        if (!s.choices.empty()) {
            // Selectors show their choice names ("saw") instead of numbers.
            attributes = attributes
                             .withStringFromValueFunction([i](float value, int) {
                                 return toJuce(synthmatch::choiceName(synthmatch::spec(i), value));
                             })
                             .withValueFromStringFunction([i](const juce::String& text) {
                                 const auto& sp = synthmatch::spec(i);
                                 const std::string name = text.trim().toLowerCase().toStdString();
                                 if (const auto value = synthmatch::parseChoice(sp, name))
                                     return *value;
                                 return synthmatch::clampToRange(sp, text.getFloatValue());
                             });
        } else if (s.curve == synthmatch::Curve::Stepped) {
            attributes = attributes.withStringFromValueFunction(
                [](float value, int) { return juce::String(juce::roundToInt(value)); });
        }

        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{toJuce(s.id), 1}, toJuce(s.name),
                                                               range, s.defaultValue, attributes));
    }

    return layout;
}

void SynthMatchProcessor::prepareToPlay(double sampleRate, int)
{
    engine_.prepare(sampleRate);
}

bool SynthMatchProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SynthMatchProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2) {
        buffer.clear();
        return;
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear(ch, 0, numSamples);

    for (int i = 0; i < synthmatch::kNumParams; ++i)
        engine_.setParam(i, rawParams_[static_cast<std::size_t>(i)]->load(std::memory_order_relaxed));

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);

    // Render up to each MIDI event so notes start on the exact sample.
    int pos = 0;
    for (const auto metadata : midi) {
        const int eventPos = std::clamp(metadata.samplePosition, 0, numSamples);
        if (eventPos > pos) {
            engine_.process(left + pos, right + pos, eventPos - pos);
            pos = eventPos;
        }
        handleMidi(metadata.getMessage());
    }
    if (pos < numSamples)
        engine_.process(left + pos, right + pos, numSamples - pos);
}

void SynthMatchProcessor::handleMidi(const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine_.noteOn(message.getNoteNumber(), message.getFloatVelocity());
    else if (message.isNoteOff())
        engine_.noteOff(message.getNoteNumber());
    else if (message.isAllSoundOff())
        engine_.allSoundOff();
    else if (message.isAllNotesOff())
        engine_.allNotesOff();
}

juce::AudioProcessorEditor* SynthMatchProcessor::createEditor()
{
    // Placeholder until Stage 2 adds the real Synth view and Stage 3 the Node view.
    return new juce::GenericAudioProcessorEditor(*this);
}

void SynthMatchProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts_.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void SynthMatchProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts_.state.getType()))
            apvts_.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SynthMatchProcessor();
}
