#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Session.h"
#include "synthmatch/Params.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <string_view>

namespace {

juce::String toJuce(std::string_view s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

constexpr const char* kExtraTag = "SynthMatchExtra";

} // namespace

SynthMatchProcessor::SynthMatchProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "SynthMatch", createLayout())
{
    for (int i = 0; i < synthmatch::kNumParams; ++i) {
        const auto id = toJuce(synthmatch::spec(i).id);
        params_[static_cast<std::size_t>(i)] = apvts_.getParameter(id);
        rawParams_[static_cast<std::size_t>(i)] = apvts_.getRawParameterValue(id);
        jassert(params_[static_cast<std::size_t>(i)] != nullptr && rawParams_[static_cast<std::size_t>(i)] != nullptr);
    }
    engineParams_ = synthmatch::defaultValues();
    for (auto& g : matchEq_)
        g.store(0.0f);
    session_ = std::make_unique<Session>(*this);
}

SynthMatchProcessor::~SynthMatchProcessor() = default;

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
                                 if (const auto value = synthmatch::parseChoice(sp, text.trim().toStdString()))
                                     return *value;
                                 return synthmatch::clampToRange(sp, text.getFloatValue());
                             });
        } else if (s.curve == synthmatch::Curve::Stepped) {
            attributes = attributes.withStringFromValueFunction(
                [](float value, int) { return juce::String(juce::roundToInt(value)); });
        } else {
            attributes = attributes.withStringFromValueFunction([](float value, int) {
                const float a = std::abs(value);
                return juce::String(value, a >= 100.0f ? 0 : (a >= 10.0f ? 1 : 2));
            });
        }

        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{toJuce(s.id), 1}, toJuce(s.name),
                                                               range, s.defaultValue, attributes));
    }

    return layout;
}

void SynthMatchProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate_.store(sampleRate);
    engine_.prepare(sampleRate);
    engine_.setParams(engineParams_);
    matchEqSeen_ = matchEqVersion_.load() - 1; // makes the next block pass the match EQ to the engine
}

bool SynthMatchProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SynthMatchProcessor::takePreviewRequest()
{
    const juce::SpinLock::ScopedTryLockType lock(previewLock_);
    if (!lock.isLocked() || pendingRequest_ == previewSeen_)
        return;
    previewSeen_ = pendingRequest_;

    // Release whatever the previous preview was still holding.
    for (int i = 0; i < previewCount_; ++i) {
        const auto& n = previewNotes_[static_cast<std::size_t>(i)];
        if (n.on <= previewPos_ && n.off > previewPos_)
            engine_.noteOff(n.note);
    }

    previewCount_ = pendingCount_;
    std::copy_n(pendingNotes_.begin(), pendingCount_, previewNotes_.begin());
    previewPos_ = 0;
    previewEnd_ = 0;
    for (int i = 0; i < previewCount_; ++i)
        previewEnd_ = std::max(previewEnd_, previewNotes_[static_cast<std::size_t>(i)].off);
    previewEnd_ += static_cast<std::int64_t>(pendingTail_ * currentSampleRate_.load());
    previewing_.store(previewCount_ > 0);
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

    // Parameters: one engine update per block, however many changed.
    synthmatch::ParamValues values;
    bool changed = false;
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = rawParams_[i]->load(std::memory_order_relaxed);
        changed = changed || values[i] != engineParams_[i];
    }
    if (changed) {
        engine_.setParams(values);
        engineParams_ = values;
    }

    if (const auto version = matchEqVersion_.load(std::memory_order_acquire); version != matchEqSeen_) {
        synthmatch::MatchEqGains gains;
        for (std::size_t b = 0; b < gains.size(); ++b)
            gains[b] = matchEq_[b].load(std::memory_order_relaxed);
        engine_.setMatchEq(gains);
        matchEqSeen_ = version;
    }

    if (auto* host = getPlayHead())
        if (const auto position = host->getPosition())
            if (const auto bpm = position->getBpm())
                engine_.setTempo(*bpm);

    takePreviewRequest();

    // This block's preview events, in order.
    struct PreviewEvent {
        int offset;
        bool on;
        int note;
        float velocity;
    };
    std::array<PreviewEvent, 2 * kMaxPreviewNotes> events{};
    int numEvents = 0;
    if (previewing_.load(std::memory_order_relaxed)) {
        const std::int64_t end = previewPos_ + numSamples;
        for (int i = 0; i < previewCount_; ++i) {
            const auto& n = previewNotes_[static_cast<std::size_t>(i)];
            if (n.off >= previewPos_ && n.off < end)
                events[static_cast<std::size_t>(numEvents++)] = {static_cast<int>(n.off - previewPos_), false, n.note, 0.0f};
            if (n.on >= previewPos_ && n.on < end)
                events[static_cast<std::size_t>(numEvents++)] = {static_cast<int>(n.on - previewPos_), true, n.note, n.velocity};
        }
        std::sort(events.begin(), events.begin() + numEvents, [](const PreviewEvent& a, const PreviewEvent& b) {
            return a.offset != b.offset ? a.offset < b.offset : (!a.on && b.on);
        });
        previewPos_ = end;
        if (previewPos_ >= previewEnd_)
            previewing_.store(false);
    }

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);

    // Render up to each event so notes start on the exact sample.
    int pos = 0, nextPreview = 0;
    auto midiIt = midi.begin();
    for (;;) {
        const int midiPos = midiIt != midi.end() ? std::clamp((*midiIt).samplePosition, 0, numSamples) : INT_MAX;
        const int previewPos = nextPreview < numEvents ? events[static_cast<std::size_t>(nextPreview)].offset : INT_MAX;
        const int eventPos = std::min(midiPos, previewPos);
        if (eventPos == INT_MAX)
            break;
        if (eventPos > pos) {
            engine_.process(left + pos, right + pos, eventPos - pos);
            pos = eventPos;
        }
        if (previewPos <= midiPos) {
            const auto& e = events[static_cast<std::size_t>(nextPreview++)];
            if (e.on)
                engine_.noteOn(e.note, e.velocity);
            else
                engine_.noteOff(e.note);
        } else {
            handleMidi((*midiIt).getMessage());
            ++midiIt;
        }
    }
    if (pos < numSamples)
        engine_.process(left + pos, right + pos, numSamples - pos);

    if (auditioning_.load(std::memory_order_relaxed))
        renderReference(left, right, numSamples);
}

void SynthMatchProcessor::renderReference(float* left, float* right, int numSamples)
{
    const juce::SpinLock::ScopedTryLockType lock(referenceLock_);
    if (!lock.isLocked() || reference_ == nullptr)
        return;

    const auto& ref = *reference_;
    const double step = ref.sampleRate / currentSampleRate_.load();
    const auto frames = static_cast<double>(ref.numFrames());
    for (int i = 0; i < numSamples; ++i) {
        if (referencePos_ >= frames - 1.0) {
            auditioning_.store(false);
            break;
        }
        const auto index = static_cast<std::size_t>(referencePos_);
        const auto t = static_cast<float>(referencePos_ - static_cast<double>(index));
        left[i] += ref.left[index] + t * (ref.left[index + 1] - ref.left[index]);
        right[i] += ref.right[index] + t * (ref.right[index + 1] - ref.right[index]);
        referencePos_ += step;
    }
}

void SynthMatchProcessor::handleMidi(const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine_.noteOn(message.getNoteNumber(), message.getFloatVelocity());
    else if (message.isNoteOff())
        engine_.noteOff(message.getNoteNumber());
    else if (message.isPitchWheel())
        engine_.setPitchBend(static_cast<float>(message.getPitchWheelValue() - 8192) / 8192.0f);
    else if (message.isAllSoundOff())
        engine_.allSoundOff();
    else if (message.isAllNotesOff())
        engine_.allNotesOff();
}

// ---------------------------------------------------------------- message thread

synthmatch::ParamValues SynthMatchProcessor::currentParams() const
{
    synthmatch::ParamValues values;
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = rawParams_[i]->load();
    return values;
}

void SynthMatchProcessor::setParams(const synthmatch::ParamValues& values)
{
    for (int i = 0; i < synthmatch::kNumParams; ++i) {
        auto& p = parameter(i);
        const float target = synthmatch::clampToRange(synthmatch::spec(i), values[static_cast<std::size_t>(i)]);
        if (rawParams_[static_cast<std::size_t>(i)]->load() == target)
            continue;
        p.beginChangeGesture();
        p.setValueNotifyingHost(p.convertTo0to1(target));
        p.endChangeGesture();
    }
}

void SynthMatchProcessor::setMatchEq(const synthmatch::MatchEqGains& gainsDb)
{
    for (std::size_t b = 0; b < gainsDb.size(); ++b)
        matchEq_[b].store(gainsDb[b], std::memory_order_relaxed);
    matchEqVersion_.fetch_add(1, std::memory_order_release);
}

synthmatch::MatchEqGains SynthMatchProcessor::matchEq() const
{
    synthmatch::MatchEqGains gains;
    for (std::size_t b = 0; b < gains.size(); ++b)
        gains[b] = matchEq_[b].load(std::memory_order_relaxed);
    return gains;
}

void SynthMatchProcessor::setReference(std::shared_ptr<const synthmatch::StereoBuffer> audio)
{
    auditioning_.store(false);
    const juce::SpinLock::ScopedLockType lock(referenceLock_);
    reference_ = std::move(audio); // the old buffer is freed here, on this thread
    referencePos_ = 0.0;
}

void SynthMatchProcessor::playReference(double fromSec)
{
    {
        const juce::SpinLock::ScopedLockType lock(referenceLock_);
        if (reference_ == nullptr)
            return;
        referencePos_ = std::max(0.0, fromSec) * reference_->sampleRate;
    }
    auditioning_.store(true);
}

void SynthMatchProcessor::playNotes(const std::vector<synthmatch::NoteEvent>& notes, double tailSec)
{
    const double rate = currentSampleRate_.load();
    const juce::SpinLock::ScopedLockType lock(previewLock_);
    pendingCount_ = 0;
    for (const auto& n : notes) {
        if (pendingCount_ >= kMaxPreviewNotes)
            break;
        auto& p = pendingNotes_[static_cast<std::size_t>(pendingCount_++)];
        p.on = static_cast<std::int64_t>(std::max(0.0, n.startSec) * rate);
        p.off = std::max(p.on + 1, static_cast<std::int64_t>((n.startSec + n.durationSec) * rate));
        p.note = n.note;
        p.velocity = n.velocity;
    }
    pendingTail_ = tailSec;
    ++pendingRequest_;
}

void SynthMatchProcessor::stopPlayback()
{
    auditioning_.store(false);
    const juce::SpinLock::ScopedLockType lock(previewLock_);
    pendingCount_ = 0;
    ++pendingRequest_;
}

juce::AudioProcessorEditor* SynthMatchProcessor::createEditor() { return new SynthMatchEditor(*this); }

void SynthMatchProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto xml = apvts_.copyState().createXml();
    if (!xml)
        return;
    auto* extra = xml->createNewChildElement(kExtraTag);
    juce::StringArray gains;
    for (const float g : matchEq())
        gains.add(juce::String(g, 3));
    extra->setAttribute("matchEq", gains.joinIntoString(","));
    session_->saveState(*extra);
    copyXmlToBinary(*xml, destData);
}

void SynthMatchProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml || !xml->hasTagName(apvts_.state.getType()))
        return;

    std::unique_ptr<juce::XmlElement> extra;
    if (auto* found = xml->getChildByName(kExtraTag)) {
        extra = std::make_unique<juce::XmlElement>(*found);
        xml->removeChildElement(found, true);
    }
    apvts_.replaceState(juce::ValueTree::fromXml(*xml));

    synthmatch::MatchEqGains gains{};
    if (extra) {
        juce::StringArray parts;
        parts.addTokens(extra->getStringAttribute("matchEq"), ",", "");
        for (int b = 0; b < std::min(parts.size(), static_cast<int>(gains.size())); ++b)
            gains[static_cast<std::size_t>(b)] = parts[b].getFloatValue();
    }
    setMatchEq(gains);
    if (extra)
        session_->loadState(*extra);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SynthMatchProcessor();
}
