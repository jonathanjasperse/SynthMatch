#include "ui/Terminal.h"

#include "Session.h"
#include "ui/Theme.h"

namespace ui {

using Kind = Session::LogEntry::Kind;

// ---------------------------------------------------------------- the scrolling log

class Terminal::Log : public juce::Component {
public:
    explicit Log(Session& session) : session_(session) {}

    void rebuild(int width, bool busy, const juce::String& busyText)
    {
        layouts_.clear();
        offsets_.clear();
        const float textWidth = static_cast<float>(width - 2 * kPad);
        float y = static_cast<float>(kPad);

        const auto addLayout = [&](const juce::AttributedString& s, float gapBefore) {
            juce::TextLayout layout;
            layout.createLayout(s, textWidth);
            y += gapBefore;
            offsets_.push_back(y);
            y += layout.getHeight();
            layouts_.push_back(std::move(layout));
        };

        Kind previous = Kind::Info;
        bool first = true;
        for (const auto& e : session_.log()) {
            juce::AttributedString s;
            s.setWordWrap(juce::AttributedString::byWord);
            s.setLineSpacing(2.0f);
            switch (e.kind) {
            case Kind::User:
                s.append(juce::String::fromUTF8("\xe2\x80\xba  "), theme::mono(13.0f), theme::colour::textDim);
                s.append(e.text, theme::mono(13.0f), theme::colour::textHi);
                break;
            case Kind::Reply:
                s.append(e.text, e.tag == "json" ? theme::mono(12.0f) : theme::sans(13.5f), theme::colour::text);
                if (e.tag.isNotEmpty() && e.tag != "json")
                    s.append(juce::String::fromUTF8("   \xc2\xb7 ") + e.tag, theme::sans(11.5f), theme::colour::textFaint);
                break;
            case Kind::Change:
                s.append("    " + e.text.replace("->", juce::String::fromUTF8("\xe2\x86\x92")), theme::mono(12.0f),
                         theme::colour::textDim);
                break;
            case Kind::Info:
                s.append(e.text, theme::sans(13.0f), theme::colour::textDim);
                break;
            case Kind::Error:
                s.append(e.text, theme::sans(13.0f), theme::colour::warn);
                break;
            }
            // A little air before each new request, none between a reply's lines.
            const float gap = first ? 0.0f : (e.kind == Kind::User ? 12.0f : (e.kind == Kind::Change && previous != Kind::User ? 1.0f : 5.0f));
            addLayout(s, gap);
            previous = e.kind;
            first = false;
        }

        if (busy) {
            juce::AttributedString s;
            s.append(busyText, theme::sans(13.0f), theme::colour::textDim);
            addLayout(s, first ? 0.0f : 5.0f);
        }

        setSize(width, juce::roundToInt(y) + kPad);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto visible = g.getClipBounds().toFloat();
        for (std::size_t i = 0; i < layouts_.size(); ++i) {
            const auto area = juce::Rectangle<float>(static_cast<float>(kPad), offsets_[i], static_cast<float>(getWidth() - 2 * kPad),
                                                     layouts_[i].getHeight());
            if (area.intersects(visible))
                layouts_[i].draw(g, area);
        }
    }

private:
    static constexpr int kPad = 14;
    Session& session_;
    std::vector<juce::TextLayout> layouts_;
    std::vector<float> offsets_;
};

// ---------------------------------------------------------------- the input line

class Terminal::Input : public juce::TextEditor {
public:
    std::function<void(int)> onHistory; // -1 older, +1 newer

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::upKey && onHistory) {
            onHistory(-1);
            return true;
        }
        if (key == juce::KeyPress::downKey && onHistory) {
            onHistory(1);
            return true;
        }
        return juce::TextEditor::keyPressed(key);
    }
};

// ---------------------------------------------------------------- terminal

Terminal::Terminal(Session& session) : session_(session)
{
    log_ = std::make_unique<Log>(session_);
    viewport_.setViewedComponent(log_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(8);
    addAndMakeVisible(viewport_);

    input_ = std::make_unique<Input>();
    input_->setFont(theme::mono(13.5f));
    input_->setTextToShowWhenEmpty(juce::String::fromUTF8("Describe a change \xe2\x80\x94 \xe2\x80\x9cmake it brighter\xe2\x80\x9d, "
                                                          "\xe2\x80\x9c" "add more reverb\xe2\x80\x9d, \xe2\x80\x9c" "cutoff 2 kHz\xe2\x80\x9d"),
                                   theme::colour::textFaint);
    input_->setIndents(0, 0);
    input_->setBorder({});
    input_->setJustification(juce::Justification::centredLeft);
    input_->onReturnKey = [this] { submit(); };
    input_->onHistory = [this](int step) {
        if (history_.empty())
            return;
        historyPos_ = juce::jlimit(0, static_cast<int>(history_.size()), historyPos_ + step);
        input_->setText(historyPos_ < static_cast<int>(history_.size()) ? history_[static_cast<std::size_t>(historyPos_)] : juce::String(),
                        false);
        input_->moveCaretToEnd();
    };
    addAndMakeVisible(*input_);

    session_.addChangeListener(this);
    refresh();
}

Terminal::~Terminal()
{
    session_.removeChangeListener(this);
}

void Terminal::focusInput() { input_->grabKeyboardFocus(); }

void Terminal::submit()
{
    const auto text = input_->getText().trim();
    if (text.isEmpty())
        return;
    if (history_.empty() || history_.back() != text)
        history_.push_back(text);
    historyPos_ = static_cast<int>(history_.size());
    input_->clear();
    session_.submit(text);
}

void Terminal::changeListenerCallback(juce::ChangeBroadcaster*) { refresh(); }

void Terminal::refresh()
{
    const bool busy = session_.assistantBusy();
    const auto entries = session_.log().size();
    if (entries != shownEntries_ || busy != wasBusy_ || busy) {
        const bool atBottom = viewport_.getViewPositionY() + viewport_.getViewHeight() >= log_->getHeight() - 30;
        const int received = session_.assistantProgress();
        juce::String busyText = "Asking " + session_.llmSettings().model + juce::String::repeatedString(".", 1 + dots_ % 3);
        if (received > 0)
            busyText += "   " + juce::String(received) + " characters";
        log_->rebuild(std::max(100, viewport_.getMaximumVisibleWidth()), busy, busyText);
        if (atBottom || entries != shownEntries_)
            viewport_.setViewPosition(0, std::max(0, log_->getHeight() - viewport_.getViewHeight()));
        shownEntries_ = entries;
        wasBusy_ = busy;
    }
    if (busy && !isTimerRunning())
        startTimer(350);
    else if (!busy)
        stopTimer();
    repaint(getLocalBounds().removeFromTop(28));
}

void Terminal::timerCallback()
{
    ++dots_;
    refresh();
}

void Terminal::resized()
{
    auto area = getLocalBounds().reduced(1);
    area.removeFromTop(28);
    auto inputRow = area.removeFromBottom(40);
    inputRow.removeFromLeft(36);
    input_->setBounds(inputRow.reduced(0, 8).withTrimmedRight(14));
    viewport_.setBounds(area.reduced(4, 0));
    log_->rebuild(std::max(100, viewport_.getMaximumVisibleWidth()), session_.assistantBusy(), {});
    viewport_.setViewPosition(0, std::max(0, log_->getHeight() - viewport_.getViewHeight()));
}

void Terminal::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    theme::drawPanel(g, area, 12.0f, 0.86f);

    // Header: title on the left, who answers on the right.
    auto header = getLocalBounds().removeFromTop(28).reduced(16, 0);
    g.setFont(theme::caps(10.0f));
    g.setColour(theme::colour::textDim);
    g.drawText("ASSISTANT", header, juce::Justification::centredLeft, false);

    const auto status = session_.modelStatusText();
    const bool ready = session_.modelStatus() == Session::ModelStatus::Ready;
    g.setFont(theme::sans(12.0f));
    const int textWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), status) + 2;
    auto statusArea = header.removeFromRight(textWidth);
    g.setColour(ready ? theme::colour::text : theme::colour::textFaint);
    g.drawText(status, statusArea, juce::Justification::centredRight, false);
    const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({static_cast<float>(statusArea.getX() - 9), static_cast<float>(statusArea.getCentreY())});
    g.setColour(ready ? theme::colour::silverHi : theme::colour::textFaint);
    g.fillEllipse(dot);
    if (ready) {
        g.setColour(theme::colour::silverHi.withAlpha(0.2f));
        g.fillEllipse(dot.expanded(3.0f));
    }

    // Hairlines between the header, the log and the input line.
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawHorizontalLine(28, 12.0f, area.getRight() - 12.0f);
    const int inputTop = getHeight() - 41;
    g.drawHorizontalLine(inputTop, 12.0f, area.getRight() - 12.0f);

    // The prompt.
    g.setFont(theme::mono(16.0f));
    g.setColour(input_->hasKeyboardFocus(true) ? theme::colour::silverHi : theme::colour::textDim);
    g.drawText(juce::String::fromUTF8("\xe2\x80\xba"), juce::Rectangle<int>(14, inputTop, 22, 40), juce::Justification::centred, false);
}

} // namespace ui
