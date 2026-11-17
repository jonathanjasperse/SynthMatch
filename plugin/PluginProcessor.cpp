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

std::size_t playableFrames(const synthmatch::StereoBuffer& audio)
{
    return std::min(audio.left.size(), audio.right.size());
}

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
    blockRate_ = sampleRate;
    engine_.prepare(sampleRate);
    engine_.setParams(engineParams_);
    limiter_.prepare(sampleRate);
    matchEqSeen_ = matchEqVersion_.load() - 1; // makes the next block pass the match EQ to the engine

    // The engine has just dropped its voices, so whatever was playing stops.
    // Audio isn't running now, so this thread may touch the audio thread's state.
    previewActive_ = false;
    stopReference();
    previewing_.store(false);
    // Convert the reference to this rate on the message thread, before it's played.
    triggerAsyncUpdate();
}

bool SynthMatchProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// ---------------------------------------------------------------- audio thread

void SynthMatchProcessor::takePlayRequest()
{
    if (!requests_.hasNew())
        return;

    // Release the notes the previous preview still holds. Each block plays the
    // events in [previewPos_, end) and then moves previewPos_ to end, so a note
    // is held when it started before previewPos_ and its off isn't played yet.
    if (previewActive_) {
        const auto& old = requests_.front();
        for (int i = 0; i < old.count; ++i) {
            const auto& n = old.notes[static_cast<std::size_t>(i)];
            if (n.on < previewPos_ && n.off >= previewPos_)
                engine_.noteOff(n.note);
        }
    }

    requests_.take();
    const auto& r = requests_.front();

    previewPos_ = 0;
    std::int64_t lastOff = -1;
    for (int i = 0; i < r.count; ++i)
        lastOff = std::max(lastOff, r.notes[static_cast<std::size_t>(i)].off);
    // Keep going until the last note-off has been played, then the tail.
    previewEnd_ = std::max(lastOff + 1, lastOff + static_cast<std::int64_t>(r.tailSec * blockRate_));
    previewActive_ = r.count > 0;
    previewing_.store(previewActive_, std::memory_order_relaxed);

    referenceActive_ = r.reference != nullptr && r.reference->sampleRate == blockRate_ &&
                       r.referenceGeneration == referenceGeneration_.load(std::memory_order_relaxed) &&
                       r.referenceStart < playableFrames(*r.reference);
    referencePos_ = r.referenceStart;
    auditioning_.store(referenceActive_, std::memory_order_relaxed);
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

    // Skipped while a write is under way (odd version) or if one started
    // during the copy; the next block tries again.
    if (const auto version = matchEqVersion_.load(std::memory_order_acquire);
        version != matchEqSeen_ && (version & 1u) == 0) {
        synthmatch::MatchEqGains gains;
        for (std::size_t b = 0; b < gains.size(); ++b)
            gains[b] = matchEq_[b].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (matchEqVersion_.load(std::memory_order_relaxed) == version) {
            engine_.setMatchEq(gains);
            matchEqSeen_ = version;
        }
    }

    if (auto* host = getPlayHead())
        if (const auto position = host->getPosition())
            if (const auto bpm = position->getBpm())
                engine_.setTempo(*bpm);

    takePlayRequest();

    // This block's preview events, in order.
    struct PreviewEvent {
        int offset;
        bool on;
        int note;
        float velocity;
    };
    std::array<PreviewEvent, 2 * kMaxPreviewNotes> events{};
    int numEvents = 0;
    if (previewActive_) {
        const auto& request = requests_.front();
        const std::int64_t end = previewPos_ + numSamples;
        for (int i = 0; i < request.count; ++i) {
            const auto& n = request.notes[static_cast<std::size_t>(i)];
            if (n.off >= previewPos_ && n.off < end)
                events[static_cast<std::size_t>(numEvents++)] = {static_cast<int>(n.off - previewPos_), false, n.note, 0.0f};
            if (n.on >= previewPos_ && n.on < end)
                events[static_cast<std::size_t>(numEvents++)] = {static_cast<int>(n.on - previewPos_), true, n.note, n.velocity};
        }
        std::sort(events.begin(), events.begin() + numEvents, [](const PreviewEvent& a, const PreviewEvent& b) {
            return a.offset != b.offset ? a.offset < b.offset : (!a.on && b.on);
        });
        previewPos_ = end;
        if (previewPos_ >= previewEnd_) {
            previewActive_ = false;
            previewing_.store(false, std::memory_order_relaxed);
        }
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
            // The raw bytes: building a MidiMessage would allocate for SysEx.
            const auto event = *midiIt;
            handleMidi(event.data, event.numBytes);
            ++midiIt;
        }
    }
    if (pos < numSamples)
        engine_.process(left + pos, right + pos, numSamples - pos);

    if (referenceActive_)
        renderReference(left, right, numSamples);

    // Last of all, so nothing that leaves the plugin can clip.
    limiter_.process(left, right, numSamples);
}

void SynthMatchProcessor::stopReference()
{
    referenceActive_ = false;
    auditioning_.store(false, std::memory_order_relaxed);
}

void SynthMatchProcessor::renderReference(float* left, float* right, int numSamples)
{
    const auto& request = requests_.front();
    // A new file, or a new device rate, stops it; the interface plays it again.
    if (request.referenceGeneration != referenceGeneration_.load(std::memory_order_relaxed) ||
        request.reference->sampleRate != blockRate_) {
        stopReference();
        return;
    }

    // Already at the device rate, so this only copies.
    const auto& ref = *request.reference;
    const auto frames = playableFrames(ref);
    const auto n = std::min(static_cast<std::size_t>(numSamples), frames - referencePos_);
    const float* fromLeft = ref.left.data() + referencePos_;
    const float* fromRight = ref.right.data() + referencePos_;
    for (std::size_t i = 0; i < n; ++i) {
        left[i] += fromLeft[i];
        right[i] += fromRight[i];
    }
    referencePos_ += n;
    if (referencePos_ >= frames)
        stopReference();
}

void SynthMatchProcessor::handleMidi(const juce::uint8* data, int numBytes)
{
    // Only channel messages (1 to 3 bytes); SysEx and meta events are ignored.
    if (numBytes < 1 || numBytes > 3)
        return;
    const int status = data[0] & 0xf0;
    const int data1 = numBytes > 1 ? data[1] : 0;
    const int data2 = numBytes > 2 ? data[2] : 0;
    switch (status) {
    case 0x90:
        if (data2 > 0) {
            engine_.noteOn(data1, static_cast<float>(data2) * (1.0f / 127.0f));
            break;
        }
        [[fallthrough]]; // a note-on with velocity 0 is a note-off
    case 0x80:
        engine_.noteOff(data1);
        break;
    case 0xe0:
        engine_.setPitchBend(static_cast<float>((data1 | (data2 << 7)) - 8192) / 8192.0f);
        break;
    case 0xb0:
        if (data1 == 120)
            engine_.allSoundOff();
        else if (data1 == 123)
            engine_.allNotesOff();
        break;
    default:
        break;
    }
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
    // Odd while writing, even when done (see processBlock).
    matchEqVersion_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
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

namespace {

// The reference at `rate`, resampled with a proper low-pass if the file's rate
// differs. Message thread only: it takes a moment for a long file.
std::shared_ptr<const synthmatch::StereoBuffer> atRate(const std::shared_ptr<const synthmatch::StereoBuffer>& source,
                                                       double rate)
{
    if (source == nullptr || source->sampleRate == rate)
        return source;
    auto out = std::make_shared<synthmatch::StereoBuffer>();
    out->sampleRate = rate;
    rt::resample(source->left.data(), source->right.data(), std::min(source->left.size(), source->right.size()),
                 source->sampleRate, rate, out->left, out->right);
    return out;
}

} // namespace

SynthMatchProcessor::PlayRequest& SynthMatchProcessor::newRequest()
{
    // This thread's own slot: the audio thread can't see it until it's sent.
    auto& r = requests_.back();
    r.count = 0;
    r.tailSec = 0.0;
    r.reference.reset();
    r.referenceStart = 0;
    r.referenceGeneration = 0;
    return r;
}

void SynthMatchProcessor::sendRequest()
{
    requests_.publish();
    // The slot this thread gets back may still hold an old reference: release
    // it here, so the audio thread never frees anything.
    requests_.back().reference.reset();
}

void SynthMatchProcessor::setReference(std::shared_ptr<const synthmatch::StereoBuffer> audio)
{
    JUCE_ASSERT_MESSAGE_THREAD
    // A new generation stops the old file on the audio thread, which keeps
    // reading it until then. Old buffers are released on this thread once no
    // request holds them.
    referenceGeneration_.fetch_add(1, std::memory_order_relaxed);
    auditioning_.store(false);
    referenceSource_ = std::move(audio);
    referencePlayback_ = atRate(referenceSource_, currentSampleRate_.load());
}

void SynthMatchProcessor::handleAsyncUpdate()
{
    // Requests still holding the old copy keep it alive until it comes back
    // through the triple buffer.
    if (const double rate = currentSampleRate_.load(); referencePlayback_ != nullptr && referencePlayback_->sampleRate != rate)
        referencePlayback_ = atRate(referenceSource_, rate);
}

void SynthMatchProcessor::playReference(double fromSec)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (referenceSource_ == nullptr)
        return;
    // In case the rate changed and the async update hasn't run yet.
    if (const double rate = currentSampleRate_.load(); referencePlayback_ == nullptr || referencePlayback_->sampleRate != rate)
        referencePlayback_ = atRate(referenceSource_, rate);

    auto& request = newRequest();
    request.reference = referencePlayback_;
    request.referenceStart = static_cast<std::size_t>(std::max(0.0, fromSec) * referencePlayback_->sampleRate);
    request.referenceGeneration = referenceGeneration_.load(std::memory_order_relaxed);
    // The flags go first, so the audio thread's answer to this request is the
    // last word (it clears them if it can't play it).
    previewing_.store(false);
    auditioning_.store(true);
    sendRequest();
}

void SynthMatchProcessor::playNotes(const std::vector<synthmatch::NoteEvent>& notes, double tailSec)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const double rate = currentSampleRate_.load();
    auto& request = newRequest();
    for (const auto& n : notes) {
        if (request.count >= kMaxPreviewNotes)
            break;
        auto& p = request.notes[static_cast<std::size_t>(request.count++)];
        p.on = static_cast<std::int64_t>(std::max(0.0, n.startSec) * rate);
        p.off = std::max(p.on + 1, static_cast<std::int64_t>((n.startSec + n.durationSec) * rate));
        p.note = n.note;
        p.velocity = n.velocity;
    }
    request.tailSec = tailSec;
    auditioning_.store(false);
    previewing_.store(request.count > 0);
    sendRequest();
}

void SynthMatchProcessor::stopPlayback()
{
    JUCE_ASSERT_MESSAGE_THREAD
    newRequest();
    auditioning_.store(false);
    previewing_.store(false);
    sendRequest();
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
