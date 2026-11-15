#pragma once

#include "ui/MainView.h"
#include "ui/NodeView.h"
#include "ui/About.h"
#include "ui/Terminal.h"
#include "ui/Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

class SynthMatchProcessor;
class Session;

// The plugin window: a top bar, the main view or the node view, and the
// assistant terminal along the bottom.
class SynthMatchEditor final : public juce::AudioProcessorEditor,
                               public juce::FileDragAndDropTarget,
                               private juce::ChangeListener,
                               private juce::Timer {
public:
    explicit SynthMatchEditor(SynthMatchProcessor& processor);
    ~SynthMatchEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void showNodes(bool show);
    void chooseReference();
    void openPreset();
    void savePreset();
    void openFile(const juce::File& file);
    void runSnapshotStep();

    SynthMatchProcessor& processor_;
    Session& session_;
    theme::MetalLookAndFeel lookAndFeel_;
    juce::Image backdrop_;
    float backdropScale_ = 0.0f;

    ui::IconButton add_{"", ui::IconButton::Icon::Plus};
    ui::IconButton open_{"", ui::IconButton::Icon::Open};
    ui::IconButton save_{"", ui::IconButton::Icon::Save};
    ui::IconButton edit_{"Edit", ui::IconButton::Icon::Nodes};
    ui::IconButton back_{"Back", ui::IconButton::Icon::Back};
    ui::IconButton info_{"", ui::IconButton::Icon::Info};
    juce::Label presetName_;

    std::unique_ptr<ui::MainView> main_;
    std::unique_ptr<ui::NodeView> nodes_;
    std::unique_ptr<ui::Terminal> terminal_;
    ui::About about_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::TooltipWindow tooltips_{this, 700};

    // Screenshots for development: set SYNTHMATCH_SNAPSHOT_DIR (and optionally
    // SYNTHMATCH_SNAPSHOT_REFERENCE) and run the standalone app.
    juce::File snapshotDir_;
    int snapshotStep_ = -1;
    int snapshotWait_ = 0;
    juce::StringArray pendingCommands_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthMatchEditor)
};
