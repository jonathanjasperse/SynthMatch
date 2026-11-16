#include "PluginEditor.h"

#include "PluginProcessor.h"
#include "Session.h"
#include "ui/Backdrop.h"

namespace {

constexpr int kTopBar = 64;
constexpr int kTerminal = 196;

} // namespace

SynthMatchEditor::SynthMatchEditor(SynthMatchProcessor& processor)
    : AudioProcessorEditor(processor), processor_(processor), session_(processor.session())
{
    setLookAndFeel(&lookAndFeel_);
    tooltips_.setLookAndFeel(&lookAndFeel_);

    main_ = std::make_unique<ui::MainView>(processor_, session_);
    main_->onChooseFile = [this] { chooseReference(); };
    nodes_ = std::make_unique<ui::NodeView>(processor_, session_);
    terminal_ = std::make_unique<ui::Terminal>(session_);
    addAndMakeVisible(*main_);
    addChildComponent(*nodes_);
    addAndMakeVisible(*terminal_);

    for (auto* b : {&add_, &open_, &save_, &edit_, &back_, &info_})
        addAndMakeVisible(*b);
    add_.setTooltip("Add a recording to match");
    open_.setTooltip("Open a preset (.json)");
    save_.setTooltip("Save the preset (.json)");
    edit_.setTooltip("Open the node editor");
    back_.setTooltip("Back to the main view");
    add_.onClick = [this] { chooseReference(); };
    open_.onClick = [this] { openPreset(); };
    save_.onClick = [this] { savePreset(); };
    edit_.onClick = [this] { showNodes(true); };
    back_.onClick = [this] { showNodes(false); };
    info_.setTooltip("About SynthMatch, its licence and source code");
    info_.onClick = [this] {
        about_.setVisible(true);
        about_.toFront(true);
        about_.grabKeyboardFocus();
    };
    addChildComponent(about_);

    presetName_.setFont(theme::light(17.0f));
    presetName_.setColour(juce::Label::textColourId, theme::colour::text);
    presetName_.setColour(juce::Label::textWhenEditingColourId, theme::colour::textHi);
    presetName_.setColour(juce::Label::outlineWhenEditingColourId, juce::Colours::transparentBlack);
    presetName_.setColour(juce::Label::backgroundWhenEditingColourId, juce::Colour(0x22ffffff));
    presetName_.setEditable(false, true, false);
    presetName_.setTooltip("Double-click to rename");
    presetName_.onTextChange = [this] { session_.setPresetName(presetName_.getText()); };
    addAndMakeVisible(presetName_);

    session_.addChangeListener(this);

    setResizable(true, true);
    setResizeLimits(760, 560, 1900, 1300);
    setSize(1120, 780);
    showNodes(session_.showNodes);
    changeListenerCallback(nullptr);

    designStamp_ = theme::designFilesStamp();
    if (designStamp_.isNotEmpty()) {
        designWatch_ = std::make_unique<juce::TimedCallback>([this] {
            const auto stamp = theme::designFilesStamp();
            if (stamp != designStamp_) {
                designStamp_ = stamp;
                backdrop_ = {};
                repaint();
            }
        });
        designWatch_->startTimer(1000);
    }

    const auto dir = juce::SystemStats::getEnvironmentVariable("SYNTHMATCH_SNAPSHOT_DIR", {});
    if (dir.isNotEmpty()) {
        // SYNTHMATCH_SNAPSHOT_SIZE=WxH checks the layout at another size.
        juce::StringArray size;
        size.addTokens(juce::SystemStats::getEnvironmentVariable("SYNTHMATCH_SNAPSHOT_SIZE", {}), "x", "");
        if (size.size() == 2 && size[0].getIntValue() > 0 && size[1].getIntValue() > 0)
            setSize(size[0].getIntValue(), size[1].getIntValue());
        snapshotDir_ = juce::File(dir);
        snapshotDir_.createDirectory();
        snapshotStep_ = 0;
        startTimer(250);
    }
}

SynthMatchEditor::~SynthMatchEditor()
{
    session_.removeChangeListener(this);
    tooltips_.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void SynthMatchEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (!presetName_.isBeingEdited())
        presetName_.setText(session_.presetName(), juce::dontSendNotification);
}

void SynthMatchEditor::showNodes(bool show)
{
    session_.showNodes = show;
    main_->setVisible(!show);
    nodes_->setVisible(show);
    edit_.setVisible(!show);
    back_.setVisible(show);
    repaint();
}

void SynthMatchEditor::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(kTopBar).reduced(20, 14);

    auto right = top.removeFromRight(372);
    const int editWidth = std::max(edit_.idealWidth(), back_.idealWidth());
    edit_.setBounds(right.removeFromRight(editWidth));
    back_.setBounds(edit_.getBounds());
    right.removeFromRight(12);
    add_.setBounds(right.removeFromRight(right.getHeight()).expanded(2));
    right.removeFromRight(14);
    save_.setBounds(right.removeFromRight(right.getHeight()));
    right.removeFromRight(6);
    open_.setBounds(right.removeFromRight(right.getHeight()));
    right.removeFromRight(6);
    info_.setBounds(right.removeFromRight(right.getHeight()));

    top.removeFromLeft(212); // the mark and name
    presetName_.setBounds(top.removeFromLeft(std::min(360, top.getWidth())));

    // The terminal gives up some height in a small window.
    const int terminalHeight = juce::jlimit(140, kTerminal, getHeight() / 4);
    terminal_->setBounds(area.removeFromBottom(terminalHeight).reduced(20, 0).withTrimmedBottom(16));
    area.removeFromBottom(6);
    main_->setBounds(area);
    nodes_->setBounds(area);
    about_.setBounds(getLocalBounds());
    backdrop_ = {};
}

void SynthMatchEditor::paint(juce::Graphics& g)
{
    const float scale = static_cast<float>(g.getInternalContext().getPhysicalPixelScaleFactor());
    if (!backdrop_.isValid() || backdropScale_ != scale || backdrop_.getWidth() != juce::roundToInt(getWidth() * scale)) {
        backdropScale_ = scale;
        backdrop_ = theme::renderBackdrop(getWidth(), getHeight(), scale);
    }
    g.drawImage(backdrop_, getLocalBounds().toFloat());

    // Top bar: the mark, the name, and a hairline beneath.
    auto bar = getLocalBounds().removeFromTop(kTopBar).reduced(20, 0);
    theme::drawMark(g, juce::Rectangle<float>(30.0f, 20.0f).withCentre({static_cast<float>(bar.getX() + 15), static_cast<float>(bar.getCentreY())}));
    g.setFont(theme::caps(13.0f));
    g.setGradientFill(theme::silver(bar.toFloat().withSizeKeepingCentre(static_cast<float>(bar.getWidth()), 16.0f)));
    g.drawText("SYNTHMATCH", bar.withTrimmedLeft(42).removeFromLeft(150), juce::Justification::centredLeft, false);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.drawVerticalLine(bar.getX() + 194, static_cast<float>(bar.getCentreY() - 10), static_cast<float>(bar.getCentreY() + 10));

    juce::ColourGradient hairline(juce::Colours::white.withAlpha(0.0f), 0.0f, 0.0f, juce::Colours::white.withAlpha(0.0f),
                                  static_cast<float>(getWidth()), 0.0f, false);
    hairline.addColour(0.2, juce::Colours::white.withAlpha(0.09f));
    hairline.addColour(0.8, juce::Colours::white.withAlpha(0.09f));
    g.setGradientFill(hairline);
    g.fillRect(0.0f, static_cast<float>(kTopBar) - 1.0f, static_cast<float>(getWidth()), 1.0f);
}

bool SynthMatchEditor::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && key.getKeyCode() == 'Z') {
        mods.isShiftDown() ? session_.redo() : session_.undo();
        return true;
    }
    if (mods.isCommandDown() && key.getKeyCode() == 'Y') {
        session_.redo();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- files

void SynthMatchEditor::chooseReference()
{
    chooser_ = std::make_unique<juce::FileChooser>("Choose a recording of a synth",
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                   session_.audioFileWildcard());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& chooser) {
                              const auto file = chooser.getResult();
                              if (file.existsAsFile())
                                  openFile(file);
                          });
}

void SynthMatchEditor::openPreset()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open a preset", session_.presetFolder(), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& chooser) {
                              const auto file = chooser.getResult();
                              if (file.existsAsFile())
                                  openFile(file);
                          });
}

void SynthMatchEditor::savePreset()
{
    const auto name = juce::File::createLegalFileName(session_.presetName().isEmpty() ? "Preset" : session_.presetName());
    chooser_ = std::make_unique<juce::FileChooser>("Save the preset", session_.presetFolder().getChildFile(name + ".json"), "*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& chooser) {
                              auto file = chooser.getResult();
                              if (file == juce::File())
                                  return;
                              if (!file.hasFileExtension("json"))
                                  file = file.withFileExtension("json");
                              juce::String error;
                              if (!session_.savePresetFile(file, error))
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Couldn't save the preset", error);
                          });
}

void SynthMatchEditor::openFile(const juce::File& file)
{
    juce::String error;
    if (file.hasFileExtension("json")) {
        if (!session_.loadPresetFile(file, error))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't open the preset", error);
        return;
    }
    showNodes(false);
    if (!session_.loadReference(file, error))
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Couldn't open the recording", error);
}

bool SynthMatchEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (session_.isAudioFile(f) || f.endsWithIgnoreCase(".json"))
            return true;
    return false;
}

void SynthMatchEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    showNodes(false);
    main_->setDragHover(true);
}

void SynthMatchEditor::fileDragExit(const juce::StringArray&) { main_->setDragHover(false); }

void SynthMatchEditor::filesDropped(const juce::StringArray& files, int, int)
{
    main_->setDragHover(false);
    for (const auto& f : files) {
        if (session_.isAudioFile(f) || f.endsWithIgnoreCase(".json")) {
            openFile(juce::File(f));
            return;
        }
    }
}

// ---------------------------------------------------------------- screenshots

void SynthMatchEditor::timerCallback() { runSnapshotStep(); }

void SynthMatchEditor::runSnapshotStep()
{
    const auto shoot = [this](const juce::String& name) {
        const auto image = createComponentSnapshot(getLocalBounds(), true, 1.0f);
        juce::FileOutputStream out(snapshotDir_.getChildFile(name + ".png"));
        if (out.openedOk()) {
            out.setPosition(0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream(image, out);
        }
    };

    switch (snapshotStep_) {
    case 0:
        shoot("1-main-empty");
        about_.setVisible(true);
        about_.toFront(false);
        snapshotStep_ = 10;
        break;
    case 10:
        shoot("1b-about");
        about_.setVisible(false);
        snapshotStep_ = 1;
        break;
    case 1: {
        const juce::File reference(juce::SystemStats::getEnvironmentVariable("SYNTHMATCH_SNAPSHOT_REFERENCE", {}));
        juce::String error;
        if (reference.existsAsFile() && session_.loadReference(reference, error)) {
            snapshotStep_ = 2;
            snapshotWait_ = 0;
        } else {
            snapshotStep_ = 4;
        }
        break;
    }
    case 2:
        if (session_.matchState() == Session::MatchState::Running) {
            if (++snapshotWait_ == 6)
                shoot("2-main-matching");
            break;
        }
        {
            // Terminal input to try, separated by semicolons (or a default pair).
            juce::StringArray commands;
            commands.addTokens(juce::SystemStats::getEnvironmentVariable("SYNTHMATCH_SNAPSHOT_COMMANDS",
                                                                         "make it brighter and add some reverb;it should feel like it's underwater"),
                               ";", "");
            pendingCommands_ = commands;
        }
        snapshotStep_ = 3;
        snapshotWait_ = 0;
        break;
    case 3:
        if (session_.assistantBusy() && ++snapshotWait_ < 480)
            break;
        if (!pendingCommands_.isEmpty()) {
            session_.submit(pendingCommands_[0]);
            pendingCommands_.remove(0);
            snapshotWait_ = 0;
            break;
        }
        shoot("3-main-matched");
        snapshotStep_ = 4;
        break;
    case 4:
        showNodes(true);
        nodes_->select({});
        snapshotStep_ = 5;
        break;
    case 5:
        shoot("4-nodes");
        nodes_->select("delay");
        snapshotStep_ = 6;
        break;
    case 6:
        shoot("5-nodes-delay");
        stopTimer();
        if (juce::JUCEApplicationBase::isStandaloneApp())
            juce::JUCEApplicationBase::quit();
        break;
    default:
        stopTimer();
        break;
    }
}
