#pragma once

// Everything in the plugin that isn't audio: the reference recording, the
// match (run on a background thread), presets and their undo history, and the
// assistant terminal. Owned by the processor, so it outlives the editor window.
// All public functions are for the message thread.

#include "LlmClient.h"
#include "synthmatch/Preset.h"
#include "synthmatch/Render.h"
#include "synthmatch/assist/Assistant.h"
#include "synthmatch/match/Encoder.h"
#include "synthmatch/match/Matcher.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class SynthMatchProcessor;

class Session : public juce::ChangeBroadcaster, private juce::Timer {
public:
    explicit Session(SynthMatchProcessor& processor);
    ~Session() override;

    // ---- Preset
    synthmatch::Preset preset() const;
    juce::String presetName() const;
    void setPresetName(const juce::String& name);
    // Both are undoable; `label` names the change in the undo history.
    void applyPreset(const synthmatch::Preset& preset, const juce::String& label);
    std::vector<synthmatch::assist::Change> applyEdits(const std::vector<synthmatch::assist::Edit>& edits,
                                                      const juce::String& label);
    // Records the current state before a change made elsewhere (for undo).
    void checkpoint(const juce::String& label);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    juce::String undo(); // what was undone, or empty if nothing
    juce::String redo();
    bool savePresetFile(const juce::File& file, juce::String& error);
    bool loadPresetFile(const juce::File& file, juce::String& error);
    juce::File presetFolder() const;

    // ---- Reference recording
    struct Reference {
        juce::File file;
        std::shared_ptr<const synthmatch::StereoBuffer> audio;
        std::vector<float> peaks; // (min, max) pairs across the whole file
        double seconds = 0.0;
    };
    static constexpr int kOverviewPoints = 800;
    bool isAudioFile(const juce::String& path) const;
    juce::String audioFileWildcard() const { return formats_.getWildcardForAllFormats(); }
    bool loadReference(const juce::File& file, juce::String& error, bool startMatching = true);
    bool hasReference() const { return reference_.audio != nullptr; }
    // Removes the recording and its match; the current sound stays.
    void clearReference();
    const Reference& reference() const { return reference_; }

    // ---- Matching
    enum class MatchState { Idle, Running, Done, Failed };
    MatchState matchState() const { return matchState_; }
    double matchProgress() const;
    juce::String matchStage() const;
    double liveDistance() const;
    void startMatch();
    // Plays the preset `octaves` higher (or lower) from the same keys, for
    // when MIDI comes out at the wrong octave. The match is moved with it, so
    // it sounds the same in the comparison. False if it can't go further.
    bool shiftOctave(int octaves);
    void stopMatch(); // keeps the best sound found so far
    bool hasEncoder() const { return encoder_.has_value(); }

    struct MatchSummary {
        bool valid = false;
        double score = 0.0, distance = 0.0, startDistance = 0.0, seconds = 0.0, pitchConfidence = 0.0;
        int evaluations = 0;
        bool usedEncoder = false;
        std::vector<synthmatch::NoteEvent> notes;
        double sectionStartSec = 0.0, sectionSeconds = 0.0; // the part of the reference that was matched
        synthmatch::MatchEqGains referenceSpectrum{}, synthSpectrum{}; // dB per third-octave band
        std::vector<float> referenceFine, synthFine; // dB per twelfth-octave band, for drawing
        bool hasSpectra = false;
        juce::String error;
    };
    const MatchSummary& lastMatch() const { return lastMatch_; }

    // ---- Listening
    void playReference();
    void playSynth();
    void stopPlayback();
    bool isPlaying() const;

    // ---- Assistant terminal
    struct LogEntry {
        enum class Kind { User, Reply, Change, Info, Error };
        Kind kind = Kind::Info;
        juce::String text;
        juce::String tag; // who answered: the model's name, or "json"
    };
    const std::vector<LogEntry>& log() const { return log_; }
    void submit(const juce::String& text);
    bool assistantBusy() const { return llmJob_ != nullptr || pullJob_ != nullptr; }
    juce::String busyText() const; // what the assistant is doing, for the terminal

    enum class ModelStatus { Checking, Ready, Missing, Offline };
    ModelStatus modelStatus() const { return modelStatus_; }
    juce::String modelStatusText() const;
    const llm::Settings& llmSettings() const { return llmSettings_; }

    // ---- Interface state, saved with the plugin
    juce::String nodeLayout;
    bool showNodes = false;

    void saveState(juce::XmlElement& xml) const;
    void loadState(const juce::XmlElement& xml); // may be called on any thread

private:
    struct MatchJob;
    struct LlmJob;
    struct ProbeJob;
    struct PullJob;

    void timerCallback() override;
    void setPresetNoUndo(const synthmatch::Preset& preset);
    void pushUndo(const juce::String& label);
    void finishMatch();
    void finishLlm();
    void finishProbe();
    void finishPull();
    void startPull(const juce::String& model);
    void startProbe();
    static void computeSpectra(MatchSummary& summary, const synthmatch::Preset& preset,
                               const std::shared_ptr<const synthmatch::StereoBuffer>& audio);
    void discardMatch(); // stops a running match and throws its result away
    void applyAndReport(const std::vector<synthmatch::assist::Edit>& edits, const juce::String& said,
                        const juce::String& tag);
    void runCommand(const juce::String& command);
    juce::String noModelText() const; // why a request can't be answered, and what to do
    void add(LogEntry::Kind kind, const juce::String& text, const juce::String& tag = {});
    void saveSettings();

    SynthMatchProcessor& processor_;
    juce::AudioFormatManager formats_;
    juce::ApplicationProperties settingsFile_;

    juce::String presetName_ = "Init";
    struct Snapshot {
        synthmatch::Preset preset;
        juce::String label;
    };
    std::vector<Snapshot> undo_, redo_;

    Reference reference_;
    MatchState matchState_ = MatchState::Idle;
    MatchSummary lastMatch_;
    std::unique_ptr<MatchJob> matchJob_;
    std::uint32_t matchSeed_ = 1;
    std::optional<synthmatch::match::Encoder> encoder_;

    std::vector<LogEntry> log_;
    std::unique_ptr<LlmJob> llmJob_;
    std::unique_ptr<ProbeJob> probeJob_;
    std::unique_ptr<PullJob> pullJob_;
    juce::int64 probeAtMs_ = 0; // look for the model server again at this time (0: no need)
    llm::Settings llmSettings_;
    llm::Probe probe_;
    ModelStatus modelStatus_ = ModelStatus::Checking;
    juce::int64 lastProbeMs_ = 0;
    bool toldAboutModel_ = false;
    bool modelChosen_ = false; // the person picked a model with /model
    bool wasPlaying_ = false;

    // State restored by the host, applied on the message thread.
    juce::CriticalSection pendingLock_;
    std::unique_ptr<juce::XmlElement> pendingState_;
};
