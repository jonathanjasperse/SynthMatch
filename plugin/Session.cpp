#include "Session.h"

#include "PluginProcessor.h"
#include "synthmatch/match/Features.h"

#if SYNTHMATCH_HAS_ENCODER
#include "BinaryData.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

namespace assist = synthmatch::assist;
namespace match = synthmatch::match;

namespace {

constexpr int kMaxUndo = 200;
constexpr double kMaxReferenceSeconds = 60.0;
constexpr int kMatchBudget = 8000; // per attempt
// Independent searches per match, the closest kept. One search can settle on
// a sound a second one beats easily, so the first match makes several.
constexpr int kMatchAttempts = 2;

juce::String fromStd(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

const char* noteName(int note)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((note % 12) + 12) % 12];
}

juce::String noteText(int note) { return juce::String(noteName(note)) + juce::String(note / 12 - 1); }

} // namespace

// ---------------------------------------------------------------- background jobs

struct Session::MatchJob {
    std::thread thread;
    std::atomic<bool> cancel{false}, finished{false};
    std::atomic<double> progress{0.0}, distance{0.0};
    std::atomic<const char*> stage{"Listening"};
    match::MatchResult result;
    MatchSummary summary;
};

struct Session::LlmJob {
    std::thread thread;
    std::atomic<bool> cancel{false}, finished{false};
    std::atomic<int> received{0};
    assist::Reply fallback;
    llm::Answer answer;
};

struct Session::PullJob {
    std::thread thread;
    std::atomic<bool> cancel{false}, finished{false};
    std::atomic<double> done{0.0}, total{0.0};
    juce::String model;
    juce::String error;
};

struct Session::ProbeJob {
    std::thread thread;
    std::atomic<bool> finished{false};
    llm::Settings settings;
    llm::Probe result;
};

// ---------------------------------------------------------------- setup

Session::Session(SynthMatchProcessor& processor) : processor_(processor)
{
    formats_.registerBasicFormats();

    juce::PropertiesFile::Options options;
    options.applicationName = "SynthMatch";
    options.filenameSuffix = ".settings";
    options.folderName = "Bedrot Audio";
    options.osxLibrarySubFolder = "Application Support";
    settingsFile_.setStorageParameters(options);
    if (auto* s = settingsFile_.getUserSettings()) {
        llmSettings_.server = s->getValue("llmServer", llmSettings_.server);
        llmSettings_.model = s->getValue("llmModel", llmSettings_.model);
        llmSettings_.mode = s->getValue("llmMode", llmSettings_.mode);
        modelChosen_ = s->containsKey("llmModel");
    }

#if SYNTHMATCH_HAS_ENCODER
    std::string error;
    encoder_ = match::Encoder::fromMemory(
        std::span(reinterpret_cast<const unsigned char*>(BinaryData::encoder_smm),
                  static_cast<std::size_t>(BinaryData::encoder_smmSize)),
        &error);
#endif

    add(LogEntry::Kind::Info, "Describe a change to the sound, like \"make it brighter\" or \"add more reverb\". "
                              "Type help for more.");
    startProbe();
    startTimerHz(20);
}

Session::~Session()
{
    stopTimer();
    if (matchJob_) {
        matchJob_->cancel = true;
        if (matchJob_->thread.joinable())
            matchJob_->thread.join();
    }
    if (llmJob_) {
        llmJob_->cancel = true;
        if (llmJob_->thread.joinable())
            llmJob_->thread.join();
    }
    if (probeJob_ && probeJob_->thread.joinable())
        probeJob_->thread.join();
    if (pullJob_) {
        pullJob_->cancel = true;
        if (pullJob_->thread.joinable())
            pullJob_->thread.join();
    }
}

void Session::add(LogEntry::Kind kind, const juce::String& text, const juce::String& tag)
{
    log_.push_back({kind, text, tag});
    if (log_.size() > 400)
        log_.erase(log_.begin(), log_.begin() + 100);
    sendChangeMessage();
}

void Session::saveSettings()
{
    if (auto* s = settingsFile_.getUserSettings()) {
        s->setValue("llmServer", llmSettings_.server);
        if (modelChosen_)
            s->setValue("llmModel", llmSettings_.model);
        s->setValue("llmMode", llmSettings_.mode);
        s->saveIfNeeded();
    }
}

// ---------------------------------------------------------------- preset

synthmatch::Preset Session::preset() const
{
    synthmatch::Preset p;
    p.name = presetName_.toStdString();
    p.params = processor_.currentParams();
    p.matchEq = processor_.matchEq();
    return p;
}

juce::String Session::presetName() const { return presetName_; }

void Session::setPresetName(const juce::String& name)
{
    presetName_ = name.trim().isEmpty() ? juce::String("Untitled") : name.trim();
    sendChangeMessage();
}

void Session::setPresetNoUndo(const synthmatch::Preset& preset)
{
    processor_.setParams(preset.params);
    processor_.setMatchEq(preset.matchEq);
    presetName_ = fromStd(preset.name);
}

void Session::pushUndo(const juce::String& label)
{
    undo_.push_back({preset(), label});
    if (undo_.size() > static_cast<std::size_t>(kMaxUndo))
        undo_.erase(undo_.begin());
    redo_.clear();
}

void Session::checkpoint(const juce::String& label) { pushUndo(label); }

void Session::applyPreset(const synthmatch::Preset& preset, const juce::String& label)
{
    pushUndo(label);
    setPresetNoUndo(preset);
    sendChangeMessage();
}

std::vector<assist::Change> Session::applyEdits(const std::vector<assist::Edit>& edits, const juce::String& label)
{
    auto p = preset();
    const auto changes = assist::applyEdits(p.params, edits);
    if (!changes.empty()) {
        pushUndo(label);
        processor_.setParams(p.params);
        sendChangeMessage();
    }
    return changes;
}

juce::String Session::undo()
{
    if (undo_.empty())
        return {};
    auto snapshot = undo_.back();
    undo_.pop_back();
    redo_.push_back({preset(), snapshot.label});
    setPresetNoUndo(snapshot.preset);
    sendChangeMessage();
    return snapshot.label;
}

juce::String Session::redo()
{
    if (redo_.empty())
        return {};
    auto snapshot = redo_.back();
    redo_.pop_back();
    undo_.push_back({preset(), snapshot.label});
    setPresetNoUndo(snapshot.preset);
    sendChangeMessage();
    return snapshot.label;
}

bool Session::savePresetFile(const juce::File& file, juce::String& error)
{
    auto p = preset();
    if (presetName_ == "Init" || presetName_.isEmpty())
        p.name = file.getFileNameWithoutExtension().toStdString();
    std::string message;
    if (!synthmatch::savePreset(file.getFullPathName().toStdString(), p, &message)) {
        error = fromStd(message);
        return false;
    }
    presetName_ = fromStd(p.name);
    if (auto* s = settingsFile_.getUserSettings())
        s->setValue("presetFolder", file.getParentDirectory().getFullPathName());
    add(LogEntry::Kind::Info, "Saved " + file.getFileName());
    return true;
}

bool Session::loadPresetFile(const juce::File& file, juce::String& error)
{
    std::string message;
    std::vector<std::string> warnings;
    auto p = synthmatch::loadPreset(file.getFullPathName().toStdString(), &message, &warnings);
    if (!p) {
        error = fromStd(message);
        return false;
    }
    if (p->name.empty() || p->name == "Init")
        p->name = file.getFileNameWithoutExtension().toStdString();
    applyPreset(*p, "load " + file.getFileName());
    if (auto* s = settingsFile_.getUserSettings())
        s->setValue("presetFolder", file.getParentDirectory().getFullPathName());
    add(LogEntry::Kind::Info, "Loaded " + file.getFileName());
    for (const auto& w : warnings)
        add(LogEntry::Kind::Error, fromStd(w));
    return true;
}

juce::File Session::presetFolder() const
{
    if (auto* s = const_cast<juce::ApplicationProperties&>(settingsFile_).getUserSettings()) {
        const juce::File folder(s->getValue("presetFolder"));
        if (folder.isDirectory())
            return folder;
    }
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

// ---------------------------------------------------------------- reference

bool Session::isAudioFile(const juce::String& path) const
{
    const auto extension = juce::File(path).getFileExtension().toLowerCase();
    if (extension.isEmpty())
        return false;
    return formats_.findFormatForFileExtension(extension) != nullptr;
}

bool Session::loadReference(const juce::File& file, juce::String& error, bool startMatching)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (!reader) {
        error = "Can't read " + file.getFileName() + ". Try a WAV, AIFF, FLAC, OGG or MP3 file.";
        return false;
    }
    if (reader->sampleRate < 8000.0 || reader->lengthInSamples <= 0) {
        error = file.getFileName() + " has no usable audio.";
        return false;
    }

    const auto frames = static_cast<int>(std::min<juce::int64>(
        reader->lengthInSamples, static_cast<juce::int64>(kMaxReferenceSeconds * reader->sampleRate)));
    const int channels = static_cast<int>(std::max(1u, std::min(2u, reader->numChannels)));
    juce::AudioBuffer<float> buffer(channels, frames);
    reader->read(&buffer, 0, frames, 0, true, channels > 1);

    auto audio = std::make_shared<synthmatch::StereoBuffer>();
    audio->sampleRate = reader->sampleRate;
    audio->left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + frames);
    audio->right.assign(buffer.getReadPointer(channels - 1), buffer.getReadPointer(channels - 1) + frames);

    Reference ref;
    ref.file = file;
    ref.seconds = static_cast<double>(frames) / reader->sampleRate;
    ref.peaks.assign(static_cast<std::size_t>(2 * kOverviewPoints), 0.0f);
    for (int p = 0; p < kOverviewPoints; ++p) {
        const auto from = static_cast<std::size_t>(static_cast<juce::int64>(p) * frames / kOverviewPoints);
        const auto to = static_cast<std::size_t>(static_cast<juce::int64>(p + 1) * frames / kOverviewPoints);
        float lo = 0.0f, hi = 0.0f;
        for (std::size_t i = from; i < std::max(to, from + 1) && i < audio->left.size(); ++i) {
            const float v = 0.5f * (audio->left[i] + audio->right[i]);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        ref.peaks[static_cast<std::size_t>(2 * p)] = lo;
        ref.peaks[static_cast<std::size_t>(2 * p + 1)] = hi;
    }
    ref.audio = audio;

    discardMatch();
    reference_ = std::move(ref);
    processor_.setReference(reference_.audio);
    lastMatch_ = {};
    matchState_ = MatchState::Idle;
    sendChangeMessage();

    if (startMatching)
        startMatch();
    return true;
}

// ---------------------------------------------------------------- matching

double Session::matchProgress() const { return matchJob_ ? matchJob_->progress.load() : (matchState_ == MatchState::Done ? 1.0 : 0.0); }

juce::String Session::matchStage() const { return matchJob_ ? juce::String(matchJob_->stage.load()) : juce::String(); }

double Session::liveDistance() const { return matchJob_ ? matchJob_->distance.load() : lastMatch_.distance; }

void Session::startMatch()
{
    if (!hasReference() || matchJob_)
        return;

    auto job = std::make_unique<MatchJob>();
    auto* j = job.get();
    const auto audio = reference_.audio;
    const auto* encoder = encoder_ ? &*encoder_ : nullptr;
    // Every match, and every Match again, tries seeds not used before.
    const auto seed = matchSeed_;
    matchSeed_ += static_cast<std::uint32_t>(kMatchAttempts);
    const auto name = reference_.file.getFileNameWithoutExtension().toStdString();

    j->thread = std::thread([this, j, audio, encoder, seed, name] {
        const auto started = std::chrono::steady_clock::now();
        match::MatchOptions options;
        options.budget = kMatchBudget;
        options.attempts = kMatchAttempts;
        options.seed = seed;
        options.encoder = encoder;
        options.cancel = &j->cancel;
        options.onProgress = [j](const match::MatchProgress& p) {
            j->progress = p.fraction;
            j->distance = p.distance;
            j->stage = p.stage;
        };
        j->result = match::matchSound(*audio, options);

        auto& s = j->summary;
        if (j->result.ok) {
            j->result.preset.name = name.empty() ? "Matched" : name;
            s.valid = true;
            s.score = j->result.score();
            s.distance = j->result.distance;
            s.startDistance = j->result.startDistance;
            s.evaluations = j->result.evaluations;
            s.usedEncoder = j->result.usedEncoder;
            s.pitchConfidence = j->result.pitchConfidence;
            s.notes = j->result.notes;
            s.sectionStartSec = static_cast<double>(j->result.startSample) / audio->sampleRate;
            s.sectionSeconds = static_cast<double>(j->result.numSamples) / audio->sampleRate;
            computeSpectra(s, j->result.preset, audio);
        } else {
            s.error = fromStd(j->result.error);
        }
        s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        j->finished = true;
    });

    matchJob_ = std::move(job);
    matchState_ = MatchState::Running;
    sendChangeMessage();
}

void Session::stopMatch()
{
    if (matchJob_)
        matchJob_->cancel = true;
}

void Session::discardMatch()
{
    if (!matchJob_)
        return;
    matchJob_->cancel = true;
    if (matchJob_->thread.joinable())
        matchJob_->thread.join();
    matchJob_.reset();
    matchState_ = MatchState::Idle;
}

void Session::computeSpectra(MatchSummary& summary, const synthmatch::Preset& preset,
                             const std::shared_ptr<const synthmatch::StereoBuffer>& audio)
{
    // Runs on the match thread or the message thread, so it touches nothing but its arguments.
    if (!audio || summary.notes.empty())
        return;
    const auto first = static_cast<std::size_t>(summary.sectionStartSec * audio->sampleRate);
    const auto count = std::min(audio->numFrames() - std::min(first, audio->numFrames()),
                                static_cast<std::size_t>(summary.sectionSeconds * audio->sampleRate));
    if (count < 1024)
        return;
    const std::vector<float> section =
        match::toMono(std::span(audio->left).subspan(first, count), std::span(audio->right).subspan(first, count));

    double lastOff = 0.0;
    for (const auto& n : summary.notes)
        lastOff = std::max(lastOff, n.startSec + n.durationSec);
    synthmatch::RenderSettings settings;
    settings.sampleRate = audio->sampleRate;
    settings.tailSec = std::max(0.0, summary.sectionSeconds - lastOff);
    const auto synth = synthmatch::render(preset, summary.notes, settings);

    summary.referenceSpectrum = match::thirdOctaveSpectrum(section, audio->sampleRate);
    summary.synthSpectrum = match::thirdOctaveSpectrum(match::toMono(synth.left, synth.right), audio->sampleRate);
    summary.hasSpectra = true;
}

void Session::finishMatch()
{
    auto job = std::move(matchJob_);
    if (job->thread.joinable())
        job->thread.join();

    // Searching the same recording again only replaces the match with a closer one.
    if (job->result.ok && lastMatch_.valid && job->summary.score <= lastMatch_.score) {
        matchState_ = MatchState::Done;
        add(LogEntry::Kind::Info, "Searched again (" + juce::String(job->summary.evaluations) +
                                      " more sounds): nothing closer than the current " +
                                      juce::String(juce::roundToInt(lastMatch_.score)) + "/100, so it stays.");
        sendChangeMessage();
        return;
    }

    lastMatch_ = job->summary;
    if (job->result.ok) {
        applyPreset(job->result.preset, "match " + reference_.file.getFileName());
        matchState_ = MatchState::Done;

        juce::StringArray notes;
        for (const auto& n : lastMatch_.notes)
            notes.add(noteText(n.note));
        add(LogEntry::Kind::Info, "Matched " + reference_.file.getFileName() + ": " +
                                      juce::String(juce::roundToInt(lastMatch_.score)) + "/100 from " +
                                      notes.joinIntoString(" ") + " (" + juce::String(lastMatch_.evaluations) +
                                      " sounds tried in " + juce::String(lastMatch_.seconds, 1) + " s). Ask for changes below.");
        if (lastMatch_.pitchConfidence < 0.5)
            add(LogEntry::Kind::Error, "The recording didn't have a clear pitch. Chords, drums or several instruments at "
                                       "once match poorly: a single synth playing single notes works best.");
    } else {
        matchState_ = MatchState::Failed;
        add(LogEntry::Kind::Error, lastMatch_.error);
    }
    sendChangeMessage();
}

// ---------------------------------------------------------------- listening

void Session::playReference()
{
    processor_.stopPlayback();
    processor_.playReference(lastMatch_.valid ? lastMatch_.sectionStartSec : 0.0);
    sendChangeMessage();
}

void Session::playSynth()
{
    processor_.stopPlayback();
    std::vector<synthmatch::NoteEvent> notes = lastMatch_.notes;
    if (notes.empty())
        notes = {{0.0, 0.9, 48, 0.9f}, {0.0, 0.9, 55, 0.75f}, {0.0, 0.9, 60, 0.75f}};
    processor_.playNotes(notes, 2.0);
    sendChangeMessage();
}

void Session::stopPlayback()
{
    processor_.stopPlayback();
    sendChangeMessage();
}

bool Session::isPlaying() const { return processor_.isPlayingReference() || processor_.isPlayingNotes(); }

// ---------------------------------------------------------------- assistant

juce::String Session::busyText() const
{
    if (pullJob_) {
        const double done = pullJob_->done.load(), total = pullJob_->total.load();
        if (total <= 0.0)
            return "Downloading " + pullJob_->model;
        return "Downloading " + pullJob_->model + ": " + juce::String(juce::roundToInt(100.0 * done / total)) + "% of " +
               juce::String(total / 1.0e9, 1) + " GB";
    }
    if (llmJob_) {
        const int received = llmJob_->received.load();
        return "Asking " + llmSettings_.model + (received > 0 ? "   " + juce::String(received) + " characters" : juce::String());
    }
    return {};
}

void Session::startPull(const juce::String& model)
{
    if (pullJob_) {
        add(LogEntry::Kind::Info, "Already downloading " + pullJob_->model + ".");
        return;
    }
    if (!probe_.reachable || !probe_.ollama) {
        add(LogEntry::Kind::Error, llm::installedApp().exists()
                                       ? juce::String("Ollama isn't running. Type /start to start it, then /download again.")
                                       : juce::String("Downloading a model needs Ollama. Install it from ollama.com (or choose "
                                                      "the AI assistant in the SynthMatch installer), then type /download."));
        return;
    }
    auto job = std::make_unique<PullJob>();
    job->model = model;
    auto* j = job.get();
    const auto settings = llmSettings_;
    j->thread = std::thread([j, settings] {
        j->error = llm::pull(settings, j->model, j->cancel, [j](double done, double total) {
            j->done = done;
            j->total = total;
        });
        j->finished = true;
    });
    pullJob_ = std::move(job);
    add(LogEntry::Kind::Info, "Downloading " + model + " through Ollama. You can keep working; the assistant uses the "
                                                       "built-in rules until it's done.");
}

void Session::finishPull()
{
    auto job = std::move(pullJob_);
    if (job->thread.joinable())
        job->thread.join();
    if (job->error.isEmpty()) {
        llmSettings_.model = job->model;
        add(LogEntry::Kind::Info, "Installed " + job->model + ". The assistant uses it from now on.");
    } else {
        add(LogEntry::Kind::Error, "The download didn't finish: " + job->error + ". Type /download to try again.");
    }
    modelStatus_ = ModelStatus::Checking;
    startProbe();
}

juce::String Session::modelStatusText() const
{
    switch (modelStatus_) {
    case ModelStatus::Ready:
        return llmSettings_.model;
    case ModelStatus::Checking:
        return "looking for a model";
    case ModelStatus::Missing:
        return llmSettings_.model + " not installed";
    case ModelStatus::Offline:
        return "built-in rules";
    case ModelStatus::Disabled:
        return "built-in rules";
    }
    return {};
}

void Session::startProbe()
{
    if (!llmSettings_.enabled()) {
        modelStatus_ = ModelStatus::Disabled;
        return;
    }
    if (probeJob_)
        return;
    lastProbeMs_ = juce::Time::currentTimeMillis();
    if (modelStatus_ != ModelStatus::Ready)
        modelStatus_ = ModelStatus::Checking;
    auto job = std::make_unique<ProbeJob>();
    job->settings = llmSettings_;
    auto* j = job.get();
    j->thread = std::thread([j] {
        j->result = llm::probe(j->settings);
        j->finished = true;
    });
    probeJob_ = std::move(job);
}

void Session::finishProbe()
{
    auto job = std::move(probeJob_);
    if (job->thread.joinable())
        job->thread.join();
    probe_ = job->result;

    // No model chosen yet, and the default isn't installed: use the smallest
    // one that is (up to 5 GB), since the job is small.
    if (probe_.reachable && !probe_.hasModel && !modelChosen_) {
        const auto smallest = probe_.smallest(5.0e9);
        if (smallest.isNotEmpty()) {
            llmSettings_.model = smallest;
            probe_.hasModel = true;
            add(LogEntry::Kind::Info, "Using " + smallest + ", the smallest language model installed. Pick another with /model NAME.");
        }
    }

    if (!llmSettings_.enabled())
        modelStatus_ = ModelStatus::Disabled;
    else if (!probe_.reachable)
        modelStatus_ = ModelStatus::Offline;
    else if (!probe_.hasModel)
        modelStatus_ = ModelStatus::Missing;
    else
        modelStatus_ = ModelStatus::Ready;

    if (modelStatus_ == ModelStatus::Missing && !toldAboutModel_ && !pullJob_) {
        toldAboutModel_ = true;
        add(LogEntry::Kind::Info, probe_.ollama ? "Ollama is running, but " + llmSettings_.model +
                                                      " isn't installed. Type /download to get it (about 1.4 GB), or pick "
                                                      "another with /model NAME."
                                                : "Found a model server, but " + llmSettings_.model +
                                                      " isn't on it. Pick one it has with /model NAME.");
    }
    if (modelStatus_ == ModelStatus::Offline && llm::installedApp().exists() && !toldAboutModel_) {
        toldAboutModel_ = true;
        add(LogEntry::Kind::Info, "Ollama is installed but not running. Type /start to start it.");
    }
    sendChangeMessage();
}

void Session::submit(const juce::String& rawText)
{
    const auto text = rawText.trim();
    if (text.isEmpty())
        return;
    add(LogEntry::Kind::User, text);

    if (llmJob_) {
        add(LogEntry::Kind::Info, "Still working on the last request.");
        return;
    }
    if (text.startsWithChar('/')) {
        runCommand(text.substring(1).trim());
        return;
    }

    // JSON in the preset file's format is applied directly.
    if (text.startsWithChar('{')) {
        std::string error;
        const auto root = synthmatch::Json::parse(text.toStdString(), &error);
        if (!root) {
            add(LogEntry::Kind::Error, "That isn't valid JSON: " + fromStd(error));
            return;
        }
        std::vector<std::string> problems;
        std::string reply;
        const auto edits = assist::editsFromJson(*root, &reply, &problems);
        for (const auto& p : problems)
            add(LogEntry::Kind::Error, fromStd(p));
        applyAndReport(edits, fromStd(reply), "json");
        return;
    }

    const auto current = processor_.currentParams();
    auto reply = assist::interpret(text.toStdString(), current);

    switch (reply.action) {
    case assist::Reply::Action::Undo: {
        const auto what = undo();
        add(LogEntry::Kind::Reply, what.isEmpty() ? juce::String("Nothing to undo.") : "Undid " + what + ".", "rules");
        return;
    }
    case assist::Reply::Action::Redo: {
        const auto what = redo();
        add(LogEntry::Kind::Reply, what.isEmpty() ? juce::String("Nothing to redo.") : "Redid " + what + ".", "rules");
        return;
    }
    case assist::Reply::Action::Reset: {
        synthmatch::Preset init;
        applyPreset(init, "reset");
        add(LogEntry::Kind::Reply, "Back to the initial preset. (Say undo to get the last one back.)", "rules");
        return;
    }
    case assist::Reply::Action::Help:
        add(LogEntry::Kind::Reply, fromStd(assist::helpText()) +
                                       "\nCommands: /json (show the preset file)  /model NAME  /server URL  "
                                       "/llm on|off  /status  /clear",
            "rules");
        return;
    case assist::Reply::Action::Show:
        add(LogEntry::Kind::Reply, fromStd(assist::describePreset(preset())), "rules");
        return;
    case assist::Reply::Action::Edit:
    case assist::Reply::Action::None:
        break;
    }

    // Exact requests ("cutoff 2 kHz") need no interpreting, and requests made
    // only of words the rules know are answered by them at once. Anything else
    // goes to the language model when one is running, with the rules as a
    // fallback. "/llm always" sends every request that isn't exact.
    const bool modelReady = modelStatus_ == ModelStatus::Ready && llmSettings_.enabled();
    const bool useModel = modelReady && !reply.exact && (llmSettings_.mode == "always" || !reply.complete);
    if (!useModel) {
        if (reply.action == assist::Reply::Action::None) {
            add(LogEntry::Kind::Reply, fromStd(reply.text), "rules");
        } else {
            applyAndReport(reply.edits, fromStd(reply.text), "rules");
        }
        if (!reply.complete && llmSettings_.enabled() && modelStatus_ == ModelStatus::Offline && !toldAboutModel_) {
            toldAboutModel_ = true;
            add(LogEntry::Kind::Info, llm::installedApp().exists()
                                          ? juce::String("That used the built-in rules: Ollama isn't running. Type /start to start it.")
                                          : juce::String("That used the built-in rules: no local language model is installed. "
                                                         "Choose the AI assistant in the SynthMatch installer, or install "
                                                         "Ollama from ollama.com and type /download."));
        }
        // Someone may have started the server since the last look.
        if (modelStatus_ != ModelStatus::Ready && juce::Time::currentTimeMillis() - lastProbeMs_ > 10000)
            startProbe();
        return;
    }

    auto job = std::make_unique<LlmJob>();
    job->fallback = std::move(reply);
    auto* j = job.get();
    const auto settings = llmSettings_;
    const bool ollama = probe_.ollama;
    const auto* info = probe_.find(settings.model);
    const bool noThinking = info != nullptr && info->thinking;
    const std::string user = assist::modelUserPrompt(text.toStdString(), current);
    j->thread = std::thread([j, settings, ollama, noThinking, user] {
        j->answer = llm::chat(settings, ollama, noThinking, assist::modelSystemPrompt(), user, assist::modelReplySchema(),
                              j->cancel, [j](int received) { j->received = received; });
        j->finished = true;
    });
    llmJob_ = std::move(job);
    sendChangeMessage();
}

void Session::applyAndReport(const std::vector<assist::Edit>& edits, const juce::String& said, const juce::String& tag)
{
    const auto changes = applyEdits(edits, said.isNotEmpty() ? said.trimCharactersAtEnd(".").toLowerCase() : juce::String("edit"));
    if (changes.empty()) {
        add(LogEntry::Kind::Reply, said.isNotEmpty() ? said + " (Nothing changed: those settings are already at their limit.)"
                                                     : juce::String("Nothing changed."),
            tag);
        return;
    }
    if (said.isNotEmpty())
        add(LogEntry::Kind::Reply, said, tag);
    for (const auto& c : changes)
        add(LogEntry::Kind::Change, fromStd(assist::describeChange(c)));
}

void Session::finishLlm()
{
    auto job = std::move(llmJob_);
    if (job->thread.joinable())
        job->thread.join();

    const auto& fallback = job->fallback;
    const auto useFallback = [&](const juce::String& why) {
        if (fallback.action == assist::Reply::Action::Edit) {
            add(LogEntry::Kind::Info, why + " Used the built-in rules instead.");
            applyAndReport(fallback.edits, fromStd(fallback.text), "rules");
        } else {
            add(LogEntry::Kind::Error, why);
            add(LogEntry::Kind::Reply, fromStd(fallback.text), "rules");
        }
    };

    if (!job->answer.ok) {
        // A connection problem: look again before the next request.
        modelStatus_ = ModelStatus::Offline;
        startProbe();
        useFallback(job->answer.error + ".");
        return;
    }

    std::string error;
    const auto parsed = assist::parseModelReply(job->answer.text.toStdString(), &error);
    if (!parsed) {
        useFallback("The model's answer couldn't be used (" + fromStd(error) + ").");
        return;
    }

    auto p = preset();
    if (assist::applyEdits(p.params, parsed->edits).empty() && fallback.action == assist::Reply::Action::Edit) {
        useFallback("The model's answer didn't change anything.");
        return;
    }
    applyAndReport(parsed->edits, fromStd(parsed->text), llmSettings_.model);
}

void Session::runCommand(const juce::String& command)
{
    const auto name = command.upToFirstOccurrenceOf(" ", false, false).toLowerCase();
    const auto argument = command.fromFirstOccurrenceOf(" ", false, false).trim();

    if (name == "help" || name.isEmpty()) {
        add(LogEntry::Kind::Reply,
            "/json           show the preset as it is saved (JSON you can also paste back in)\n"
            "/model NAME     use another local model, e.g. /model llama3.2:1b\n"
            "/server URL     where the model server is (default http://127.0.0.1:11434)\n"
            "/llm auto       ask the model when the built-in rules don't know every word (default)\n"
            "/llm always     ask the model for everything except exact settings\n"
            "/llm off        built-in rules only\n"
            "/download       download the model through Ollama (default qwen3:1.7b)\n"
            "/start          start Ollama if it's installed but not running\n"
            "/status         what the assistant is using\n"
            "/undo  /redo  /clear",
            "rules");
    } else if (name == "json") {
        const auto root = synthmatch::presetToJsonValue(preset());
        juce::String out = "{ \"name\": \"" + presetName_ + "\", \"nodes\": {\n";
        if (const auto* nodes = root.find("nodes"))
            for (const auto& [node, fields] : nodes->members())
                out += "  \"" + fromStd(node) + "\": " + fromStd(fields.dump(0)) + ",\n";
        out = out.trimCharactersAtEnd(",\n") + "\n} }";
        add(LogEntry::Kind::Reply, out, "json");
    } else if (name == "model") {
        if (argument.isEmpty()) {
            add(LogEntry::Kind::Reply, "Model: " + llmSettings_.model +
                                           (probe_.models.empty() ? juce::String() : "\nInstalled: " + probe_.names().joinIntoString(", ")),
                "rules");
            return;
        }
        llmSettings_.model = argument;
        modelChosen_ = true;
        toldAboutModel_ = false;
        saveSettings();
        add(LogEntry::Kind::Info, "Using model " + argument + ".");
        modelStatus_ = ModelStatus::Checking;
        startProbe();
    } else if (name == "download" || name == "pull") {
        startPull(argument.isNotEmpty() ? argument : llmSettings_.model);
    } else if (name == "start") {
        const auto app = llm::installedApp();
        if (!app.exists()) {
            add(LogEntry::Kind::Error, "Ollama isn't installed. Get it from ollama.com, or choose the AI assistant in the "
                                       "SynthMatch installer.");
        } else if (probe_.reachable) {
            add(LogEntry::Kind::Info, "Ollama is already running.");
        } else {
            app.startAsProcess();
            add(LogEntry::Kind::Info, "Starting Ollama...");
            probeAtMs_ = juce::Time::currentTimeMillis() + 4000;
            toldAboutModel_ = false;
        }
    } else if (name == "server") {
        if (argument.isEmpty()) {
            add(LogEntry::Kind::Reply, "Server: " + llmSettings_.server, "rules");
            return;
        }
        llmSettings_.server = argument;
        saveSettings();
        add(LogEntry::Kind::Info, "Model server set to " + argument + ".");
        modelStatus_ = ModelStatus::Checking;
        startProbe();
    } else if (name == "llm") {
        const auto mode = argument.toLowerCase();
        llmSettings_.mode = mode == "off" || mode == "0" || mode == "false" ? "off" : (mode == "always" ? "always" : "auto");
        saveSettings();
        add(LogEntry::Kind::Info, llmSettings_.mode == "off"      ? juce::String("Language model off: built-in rules only.")
                                  : llmSettings_.mode == "always" ? juce::String("The language model now answers every request that isn't an exact setting.")
                                                                  : juce::String("The language model answers what the built-in rules can't."));
        if (llmSettings_.enabled()) {
            modelStatus_ = ModelStatus::Checking;
            startProbe();
        } else {
            modelStatus_ = ModelStatus::Disabled;
        }
    } else if (name == "status") {
        juce::String s = "Assistant: " + modelStatusText() + " (mode: " + llmSettings_.mode + ")\nServer: " + llmSettings_.server +
                         "\nModel: " + llmSettings_.model;
        if (probe_.reachable)
            s += "\nInstalled models: " + (probe_.models.empty() ? juce::String("(not listed)") : probe_.names().joinIntoString(", "));
        else if (probe_.error.isNotEmpty())
            s += "\n" + probe_.error;
        s += juce::String("\nMatching encoder: ") + (hasEncoder() ? "built in" : "not available (matching still works)");
        add(LogEntry::Kind::Reply, s, "rules");
    } else if (name == "undo" || name == "redo") {
        const auto what = name == "undo" ? undo() : redo();
        add(LogEntry::Kind::Reply, what.isEmpty() ? "Nothing to " + name + "." : (name == "undo" ? "Undid " : "Redid ") + what + ".", "rules");
    } else if (name == "clear") {
        log_.clear();
        sendChangeMessage();
    } else {
        add(LogEntry::Kind::Error, "Unknown command /" + name + ". Type /help.");
    }
}

// ---------------------------------------------------------------- state

void Session::saveState(juce::XmlElement& xml) const
{
    xml.setAttribute("presetName", presetName_);
    xml.setAttribute("reference", reference_.file.getFullPathName());
    xml.setAttribute("nodeLayout", nodeLayout);
    xml.setAttribute("showNodes", showNodes);
    xml.setAttribute("matchSeed", static_cast<int>(matchSeed_));
    if (lastMatch_.valid) {
        xml.setAttribute("matchScore", lastMatch_.score);
        xml.setAttribute("matchEvaluations", lastMatch_.evaluations);
        xml.setAttribute("matchSeconds", lastMatch_.seconds);
        xml.setAttribute("sectionStart", lastMatch_.sectionStartSec);
        xml.setAttribute("sectionSeconds", lastMatch_.sectionSeconds);
        juce::StringArray notes;
        for (const auto& n : lastMatch_.notes)
            notes.add(juce::String(n.startSec, 4) + ":" + juce::String(n.durationSec, 4) + ":" + juce::String(n.note) + ":" +
                      juce::String(n.velocity, 3));
        xml.setAttribute("matchNotes", notes.joinIntoString(";"));
    }
}

void Session::loadState(const juce::XmlElement& xml)
{
    const juce::ScopedLock lock(pendingLock_);
    pendingState_ = std::make_unique<juce::XmlElement>(xml);
}

// ---------------------------------------------------------------- timer

void Session::timerCallback()
{
    std::unique_ptr<juce::XmlElement> state;
    {
        const juce::ScopedLock lock(pendingLock_);
        state = std::move(pendingState_);
    }
    if (state) {
        presetName_ = state->getStringAttribute("presetName", presetName_);
        nodeLayout = state->getStringAttribute("nodeLayout", nodeLayout);
        showNodes = state->getBoolAttribute("showNodes", false);
        matchSeed_ = static_cast<std::uint32_t>(std::max(1, state->getIntAttribute("matchSeed", 1)));
        const juce::File file(state->getStringAttribute("reference"));
        juce::String error;
        if (file.existsAsFile() && loadReference(file, error, false) && state->hasAttribute("matchScore")) {
            MatchSummary s;
            s.valid = true;
            s.score = state->getDoubleAttribute("matchScore");
            s.evaluations = state->getIntAttribute("matchEvaluations");
            s.seconds = state->getDoubleAttribute("matchSeconds");
            s.sectionStartSec = state->getDoubleAttribute("sectionStart");
            s.sectionSeconds = state->getDoubleAttribute("sectionSeconds");
            s.pitchConfidence = 1.0;
            juce::StringArray notes;
            notes.addTokens(state->getStringAttribute("matchNotes"), ";", "");
            for (const auto& n : notes) {
                juce::StringArray parts;
                parts.addTokens(n, ":", "");
                if (parts.size() == 4)
                    s.notes.push_back({parts[0].getDoubleValue(), parts[1].getDoubleValue(), parts[2].getIntValue(),
                                       parts[3].getFloatValue()});
            }
            computeSpectra(s, preset(), reference_.audio);
            lastMatch_ = s;
            matchState_ = MatchState::Done;
        }
        sendChangeMessage();
    }

    if (matchJob_) {
        if (matchJob_->finished)
            finishMatch();
        else
            sendChangeMessage(); // progress
    }
    if (llmJob_) {
        if (llmJob_->finished)
            finishLlm();
        else
            sendChangeMessage();
    }
    if (probeJob_ && probeJob_->finished)
        finishProbe();
    if (pullJob_) {
        if (pullJob_->finished)
            finishPull();
        else
            sendChangeMessage(); // progress
    }
    if (probeAtMs_ != 0 && juce::Time::currentTimeMillis() >= probeAtMs_) {
        probeAtMs_ = 0;
        startProbe();
    }

    const bool playing = isPlaying();
    if (playing != wasPlaying_) {
        wasPlaying_ = playing;
        sendChangeMessage();
    }
}
