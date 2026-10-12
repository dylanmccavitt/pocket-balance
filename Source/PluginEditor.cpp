// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#include "PluginEditor.h"
#include "SourceIdentity.h"

namespace
{
const juce::Colour background(0xff111716), panel(0xff1a2320), line(0xff303d36);
const juce::Colour ink(0xffedf1e8), muted(0xffa3b1a7), accent(0xffc3ec85), orange(0xffefb278);
juce::Font font(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }
void text(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds,
          float size, juce::Colour colour = ink, bool bold = false, juce::Justification alignment = juce::Justification::centredLeft)
{
    g.setColour(colour); g.setFont(font(size, bold)); g.drawText(value, bounds, alignment, true);
}
void slider(juce::Slider& s, const juce::String& suffix)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 26);
    s.setTextValueSuffix(suffix); s.setDoubleClickReturnValue(true, 0);
}
void meter(juce::Graphics& g, juce::Rectangle<float> bounds, float peak)
{
    g.setColour(background); g.fillRoundedRectangle(bounds, 3);
    const float fraction = juce::jlimit(0.0f, 1.0f, (pocket::dbFromGain(peak) + 60.0f) / 60.0f);
    g.setColour(peak >= 1 ? orange : accent.withAlpha(0.8f));
    g.fillRoundedRectangle(bounds.withWidth(std::max(0.0f, bounds.getWidth() * fraction)), 3);
}

class SourceMappingPanel final : public juce::Component
{
public:
    SourceMappingPanel(PocketBalanceProcessor& processor,
                       std::function<void(int, int, const std::array<int, pocket::laneCount>&)> save,
                       std::function<void()> unlink)
    {
        setLookAndFeel(&look);
        const auto mapping = processor.sourceMapping();
        auto field = [this](juce::TextEditor& editor, const juce::String& title, int value)
        {
            editor.setName(title); editor.setInputRestrictions(3, "0123456789");
            if (value >= 0) editor.setText(juce::String(value));
            addAndMakeVisible(editor);
        };
        field(track, "Pocket Balance Mixer track number", processor.sourceOwnerTrack());
        field(slot, "Pocket Balance effect slot number, 1 to 10",
              processor.sourceOwnerSlot() < 0 ? -1 : processor.sourceOwnerSlot() + 1);
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            field(inputs[size_t(i)], "Input " + juce::String(i + 1) + " source Mixer track number", mapping[size_t(i)]);
            names[size_t(i)] = processor.laneName(i);
        }
        for (auto* button : { &link, &disconnect, &cancel }) addAndMakeVisible(button);
        link.onClick = [this, save]
        {
            if (track.isEmpty() || slot.isEmpty() || track.getText().getIntValue() > 500
                || slot.getText().getIntValue() < 1 || slot.getText().getIntValue() > 10)
            { error = "Enter the plugin's Mixer track (0–500) and effect slot (1–10)."; repaint(); return; }
            std::array<int, pocket::laneCount> selected;
            bool any = false;
            for (int i = 0; i < pocket::laneCount; ++i)
            {
                const auto value = inputs[size_t(i)].getText();
                selected[size_t(i)] = value.isEmpty() ? -1 : value.getIntValue();
                if (selected[size_t(i)] > 500 || selected[size_t(i)] == track.getText().getIntValue())
                { error = "Each source must be a different Mixer track from the plugin's track."; repaint(); return; }
                any |= selected[size_t(i)] >= 0;
            }
            if (!any) { error = "Enter at least one source Mixer track number."; repaint(); return; }
            save(track.getText().getIntValue(), slot.getText().getIntValue() - 1, selected);
            close();
        };
        disconnect.onClick = [this, unlink] { unlink(); close(); };
        cancel.onClick = [this] { close(); };
        setSize(660, 630);
    }
    ~SourceMappingPanel() override { setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(background);
        text(g, "Follow FL Mixer names", { 24, 16, 600, 30 }, 21, ink, true);
        g.setColour(muted); g.setFont(font(13));
        g.drawFittedText("Enable the Pocket Balance Source Sync controller script in FL first.\n"
                        "Match these inputs to Wrapper > Processing > Connections.\n"
                        "Names follow the Mixer. Your roles and audio settings stay unchanged.",
                        { 24, 54, 612, 66 }, juce::Justification::topLeft, 3);
        text(g, "Plugin's Mixer track", { 24, 128, 190, 24 }, 13);
        text(g, "Effect slot", { 354, 128, 140, 24 }, 13);
        text(g, "INPUT / CURRENT LABEL", { 24, 174, 228, 20 }, 10, muted);
        text(g, "MIXER #", { 252, 174, 68, 20 }, 10, muted);
        text(g, "INPUT / CURRENT LABEL", { 354, 174, 228, 20 }, 10, muted);
        text(g, "MIXER #", { 582, 174, 68, 20 }, 10, muted);
        for (int i = 0; i < pocket::laneCount; ++i)
            text(g, juce::String(i + 1).paddedLeft('0', 2) + "  " + names[size_t(i)],
                 { 24 + (i / 8) * 330, 200 + (i % 8) * 36, 218, 28 }, 12);
        g.setColour(muted); g.setFont(font(12));
        g.drawFittedText("Leave unused inputs blank. Re-link after routing changes or preset copying.\n"
                        "This mapping saves with the project. It does not change audio routing.",
                        { 24, 498, 612, 40 }, juce::Justification::topLeft, 2);
        text(g, error, { 24, 542, 612, 24 }, 12, orange);
    }
    void resized() override
    {
        track.setBounds(224, 128, 72, 28); slot.setBounds(534, 128, 72, 28);
        for (int i = 0; i < pocket::laneCount; ++i)
            inputs[size_t(i)].setBounds(250 + (i / 8) * 330, 200 + (i % 8) * 36, 66, 28);
        disconnect.setBounds(24, 578, 150, 32); cancel.setBounds(354, 578, 110, 32);
        link.setBounds(478, 578, 158, 32);
    }
private:
    void close() { if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->exitModalState(0); }
    PocketLookAndFeel look;
    juce::TextEditor track, slot;
    std::array<juce::TextEditor, pocket::laneCount> inputs;
    std::array<juce::String, pocket::laneCount> names;
    juce::TextButton link { "Link source names" }, disconnect { "Unlink names" }, cancel { "Cancel" };
    juce::String error;
};
}

PocketLookAndFeel::PocketLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::TextButton::buttonColourId, panel);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, ink);
    setColour(juce::TextButton::textColourOnId, background);
    setColour(juce::ComboBox::backgroundColourId, background);
    setColour(juce::ComboBox::textColourId, ink);
    setColour(juce::ComboBox::outlineColourId, line);
    setColour(juce::ComboBox::arrowColourId, muted);
    setColour(juce::PopupMenu::backgroundColourId, panel);
    setColour(juce::PopupMenu::textColourId, ink);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour(juce::PopupMenu::highlightedTextColourId, background);
    setColour(juce::Slider::textBoxTextColourId, ink);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, background);
    setColour(juce::Label::textColourId, ink);
    setColour(juce::TextEditor::backgroundColourId, background);
    setColour(juce::TextEditor::textColourId, ink);
    setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.3f));
    setColour(juce::TooltipWindow::backgroundColourId, panel);
    setColour(juce::TooltipWindow::textColourId, ink);
    setColour(juce::TooltipWindow::outlineColourId, line);
}
void PocketLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& colour, bool over, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = button.getToggleState() ? accent : colour;
    if (over) fill = fill.brighter(0.09f);
    if (down) fill = fill.darker(0.1f);
    if (!button.isEnabled()) fill = fill.withAlpha(0.4f);
    g.setColour(fill); g.fillRoundedRectangle(bounds, 6);
    g.setColour(button.getToggleState() ? accent : line); g.drawRoundedRectangle(bounds, 6, 1);
}
void PocketLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
    float position, float, float, juce::Slider::SliderStyle, juce::Slider& s)
{
    const float centreY = float(y) + float(height) * 0.5f;
    g.setColour(line); g.fillRoundedRectangle(float(x), centreY - 2, float(width), 4, 2);
    const float zero = float(x) + float(width) * float(s.valueToProportionOfLength(0));
    g.setColour(accent.withAlpha(s.isEnabled() ? 0.65f : 0.2f));
    g.fillRoundedRectangle(std::min(zero, position), centreY - 2, std::abs(position - zero), 4, 2);
    g.setColour(muted); g.drawVerticalLine(int(zero), centreY - 7, centreY + 7);
    g.setColour(accent); g.fillRoundedRectangle(position - 4, centreY - 10, 8, 20, 2);
}

PocketBalanceEditor::Lane::Lane(PocketBalanceProcessor& p, int i) : owner(p), index(i)
{
    name.setText(p.laneName(i), juce::dontSendNotification); name.setEditable(false, true);
    name.setFont(font(13, true)); name.setTooltip("Source: " + p.laneName(i) + "\nUse FL source names to follow a Mixer track, or double-click to name this source manually.");
    name.onTextChange = [this]
    {
        owner.parameters.state.setProperty("autoRole" + juce::String(index), autoRole.getToggleState(), nullptr);
        owner.setLaneName(index, name.getText()); updateIdentity();
    };
    name.setName("source_" + juce::String(i));
    role.setName("role_" + juce::String(i));
    autoRole.setName("auto_role_" + juce::String(i));
    for (int r = 0; r < pocket::roleCount; ++r) role.addItem(pocket::roleNames[size_t(r)], r + 1);
    role.setTooltip("Musical role controls balance and ducking. Choose a role manually, or enable Auto to infer it from the source label.");
    autoRole.setClickingTogglesState(true);
    const auto identityKey = "autoRole" + juce::String(i);
    autoRole.setToggleState(bool(p.parameters.state.getProperty(identityKey, p.laneName(i).startsWith("Drum "))), juce::dontSendNotification);
    autoRole.onClick = [this, identityKey] { owner.parameters.state.setProperty(identityKey, autoRole.getToggleState(), nullptr); updateIdentity(); };
    autoRole.setTooltip("Infer a role when you manually edit the source label or click Auto. Automatic FL name updates preserve the current role.");
    slider(gain, " dB"); gain.setTooltip("Manual level. Double-click the fader to return to 0 dB.");
    mute.setClickingTogglesState(true); solo.setClickingTogglesState(true);
    mute.setTooltip("Mute this input."); solo.setTooltip("Listen to this input. Multiple solos are supported.");
    for (auto* component : std::initializer_list<juce::Component*> { &name, &role, &autoRole, &gain, &mute, &solo }) addAndMakeVisible(component);
    gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, p.laneId(i, "gain"), gain);
    observedRole = int(p.value(p.laneId(i, "role")));
    role.setSelectedId(observedRole + 1, juce::dontSendNotification);
    muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.parameters, p.laneId(i, "mute"), mute);
    soloAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.parameters, p.laneId(i, "solo"), solo);
    role.onChange = [this, identityKey]
    {
        const int selectedRole = role.getSelectedId() - 1;
        // A host/state refresh can supersede a queued ComboBox notification.
        // Such a stale callback must not turn Auto off on the recalled state.
        if (selectedRole == observedRole) return;
        observedRole = selectedRole;
        autoRole.setToggleState(false, juce::dontSendNotification);
        owner.parameters.state.setProperty(identityKey, false, nullptr);
        owner.setParameterValue(owner.laneId(index, "role"), float(selectedRole));
    };
    setTitle("Drum input " + juce::String(i + 1));
}
void PocketBalanceEditor::Lane::paint(juce::Graphics& g)
{
    g.setColour(index % 2 == 0 ? panel : background.brighter(0.018f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 5);
    const float peak = owner.meters[size_t(index)].load();
    g.setColour(peak > 0.0001f ? accent : line); g.fillEllipse(12, float(getHeight()) * 0.5f - 2.5f, 5, 5);
    text(g, juce::String(index + 1).paddedLeft('0', 2), { 24, 0, 30, getHeight() }, 12, muted);
    const int right = getWidth();
    meter(g, { float(right - 340), 9, 98, 6 }, peak);
    text(g, peak > 0.00001f ? juce::String(pocket::dbFromGain(peak), 1) + " dB" : "No signal",
         { right - 340, 19, 100, 15 }, 10, muted);
    text(g, suggestionText, { right - 230, 0, 108, getHeight() }, 13,
         suggestionText == "--" ? muted : accent, false, juce::Justification::centred);
}
void PocketBalanceEditor::Lane::resized()
{
    const int y = (getHeight() - 28) / 2;
    name.setBounds(57, y, 145, 28); role.setBounds(208, y, 122, 28);
    autoRole.setBounds(336, y, 45, 28);
    gain.setBounds(395, y, getWidth() - 753, 28);
    mute.setBounds(getWidth() - 102, y, 36, 28); solo.setBounds(getWidth() - 56, y, 36, 28);
}
void PocketBalanceEditor::Lane::refresh(const pocket::Suggestions& suggestions, float strength)
{
    const int modelRole = int(owner.value(owner.laneId(index, "role")));
    if (modelRole != observedRole)
    {
        observedRole = modelRole;
        role.setSelectedId(modelRole + 1, juce::dontSendNotification);
    }
    const auto key = "autoRole" + juce::String(index);
    autoRole.setToggleState(bool(owner.parameters.state.getProperty(key, owner.laneName(index).startsWith("Drum "))), juce::dontSendNotification);
    suggestionText = suggestions.valid[size_t(index)]
        ? juce::String(suggestions.gainDb[size_t(index)] * strength, 1) + " dB" : "--";
    if (!name.isBeingEdited() && name.getText() != owner.laneName(index))
    {
        name.setText(owner.laneName(index), juce::dontSendNotification);
        name.setTooltip("Source: " + owner.laneName(index) + "\n" + owner.sourceSyncStatus() + "\nAutomatic Mixer name updates preserve this role.");
    }
    repaint();
}
void PocketBalanceEditor::Lane::updateIdentity()
{
    name.setTooltip("Source: " + owner.laneName(index) + "\n" + owner.sourceSyncStatus());
    if (!autoRole.getToggleState()) return;
    const int inferred = pocket::inferRole(owner.laneName(index).toStdString());
    if (inferred >= 0 && inferred != int(owner.value(owner.laneId(index, "role"))))
    {
        owner.setParameterValue(owner.laneId(index, "role"), float(inferred));
        observedRole = inferred;
        role.setSelectedId(inferred + 1, juce::dontSendNotification);
    }
}

PocketBalanceEditor::PocketBalanceEditor(PocketBalanceProcessor& p) : AudioProcessorEditor(&p), owner(p), waveforms(p), clashes(p)
{
    setLookAndFeel(&look);
    addAndMakeVisible(waveforms);
    addChildComponent(clashes);
    analysisView.setName("analysis_view");
    analysisView.addItem("Waveforms", 1); analysisView.addItem("Frequency clashes", 2);
    analysisView.setTooltip("View live waveforms or review frequency overlap from the latest Listen capture. Switching views stops a correction preview.");
    observedAnalysisView = juce::jlimit(1, 2, int(owner.parameters.state.getProperty("analysisView", 1)));
    analysisView.setSelectedId(observedAnalysisView, juce::dontSendNotification);
    analysisView.onChange = [this]
    {
        const int selected = analysisView.getSelectedId();
        if (selected == observedAnalysisView) return;
        observedAnalysisView = selected;
        owner.parameters.state.setProperty("analysisView", selected, nullptr);
        selectAnalysisView(selected);
    };
    addAndMakeVisible(analysisView);
    selectAnalysisView(observedAnalysisView);
    for (int i = 0; i < pocket::laneCount; ++i) { lanes[size_t(i)] = std::make_unique<Lane>(p, i); addChildComponent(lanes[size_t(i)].get()); }
    for (auto* b : { &listen, &apply, &undo, &compare, &reset, &help, &sourceNames, &pageOne, &pageTwo, &guard }) addAndMakeVisible(b);
    listen.setColour(juce::TextButton::buttonColourId, accent);
    listen.setColour(juce::TextButton::textColourOffId, background);
    listen.onClick = [this] { balanceApplied = false; if (owner.isLearning()) owner.cancelLearning(); else owner.startLearning(); };
    apply.onClick = [this]
    {
        balanceApplied = owner.applyBalance(); appliedAmount = owner.value("amount");
        for (int i = 0; i < pocket::laneCount; ++i) appliedRoles[size_t(i)] = owner.value(owner.laneId(i, "role"));
    };
    undo.onClick = [this] { balanceApplied = false; owner.undoBalance(); };
    reset.onClick = [this] { balanceApplied = false; owner.resetLevels(); };
    compare.setClickingTogglesState(true);
    compare.onClick = [this] { owner.stopClashPreview(); owner.compare.store(compare.getToggleState()); };
    compare.setTooltip("Bypass balance, kick ducking, and frequency correction while keeping all routed drums audible. Output trim, peak guard, mute and solo stay active. Use this instead of FL's plugin power switch, which drops auxiliary inputs.");
    reset.setTooltip("Reset all drum levels, mute, and solo. Undo restores the previous levels.");
    help.onClick = [this] { showRouting(); };
    sourceNames.onClick = [this] { showSourceMapping(); };
    sourceNames.setTooltip("Link each input to its FL Mixer track. Requires the Pocket Balance Source Sync controller script.");
    sourceSyncStatus.setName("FL source name sync status");
    sourceSyncStatus.setFont(font(10)); sourceSyncStatus.setColour(juce::Label::textColourId, muted);
    addAndMakeVisible(sourceSyncStatus);
    pageOne.onClick = [this] { selectPage(0); }; pageTwo.onClick = [this] { selectPage(1); };
    guard.setClickingTogglesState(true);
    guard.setTooltip("Stereo-linked, zero-latency sample-peak protection at -1 dBFS. Heavy reduction can change transients. Not true-peak limiting.");
    slider(amount, " %"); slider(duck, " dB"); slider(output, " dB");
    amount.setDoubleClickReturnValue(true, 70); output.setDoubleClickReturnValue(true, -6);
    amount.setTooltip("Scale the suggested fader levels before Apply. This control does not alter levels already applied.");
    duck.setTooltip("Reduce 808 / Bass lanes when audible Kick lanes hit. 1 ms attack, 90 ms release. Zero disables ducking.");
    output.setTooltip("Final output trim. Starts at -6 dB to allow room for summing drums.");
    for (auto* s : { &amount, &duck, &output }) addAndMakeVisible(s);
    amountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "amount", amount);
    duckAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "duck", duck);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "output", output);
    guardAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(p.parameters, "guard", guard);
    setResizable(true, true); setResizeLimits(1120, 880, 1600, 1100); setSize(1200, 900);
    selectPage(0); timerCallback(); startTimerHz(30);
}
PocketBalanceEditor::~PocketBalanceEditor() { stopTimer(); owner.stopClashPreview(); setLookAndFeel(nullptr); }
void PocketBalanceEditor::selectAnalysisView(int value)
{
    owner.stopClashPreview();
    waveforms.setVisible(value == 1); clashes.setVisible(value == 2);
    clashes.refresh();
}
void PocketBalanceEditor::selectPage(int value)
{
    page = value; pageOne.setToggleState(page == 0, juce::dontSendNotification); pageTwo.setToggleState(page == 1, juce::dontSendNotification);
    for (int i = 0; i < pocket::laneCount; ++i) lanes[size_t(i)]->setVisible(i / 8 == page);
    resized();
}
void PocketBalanceEditor::paint(juce::Graphics& g)
{
    g.fillAll(background);
    const int w = getWidth();
    text(g, "POCKET", { 24, 8, 225, 42 }, 32, ink, true);
    text(g, "B A L A N C E", { 26, 47, 210, 21 }, 11, accent);
    text(g, "See the groove. Find the space.", { 235, 19, 460, 27 }, 19, muted);
    text(g, "16 INPUTS  /  STEREO OUT", { 235, 48, 245, 18 }, 10, muted);
    text(g, "OUTPUT", { w - 268, 24, 80, 20 }, 11, muted);
    const float peak = owner.outputPeak.load();
    text(g, peak > 0.00001f ? juce::String(pocket::dbFromGain(peak), 1) + " dBFS" : "-- dBFS",
         { w - 176, 21, 142, 26 }, 19, peak > 1 ? orange : accent, true, juce::Justification::centredRight);
    meter(g, { float(w - 268), 55, 234, 7 }, peak);

    g.setColour(panel); g.fillRoundedRectangle(24, 306, float(w - 48), 74, 10);
    text(g, status, { 562, 316, w - 604, 28 }, 13, ink);
    text(g, "Listen, review, then Apply. Adjust to taste.", { 562, 347, w - 604, 18 }, 11, muted);
    if (owner.isLearning())
    {
        g.setColour(line); g.fillRoundedRectangle(40, 373, float(w - 80), 3, 1.5f);
        g.setColour(accent); g.fillRoundedRectangle(40, 373, float(w - 80) * owner.progress(), 3, 1.5f);
    }
    text(g, "YOUR DRUMS", { 24, 391, 145, 30 }, 12, muted, true);
    text(g, "SOURCE / NAME", { 82, 426, 180, 18 }, 10, muted);
    text(g, "ROLE", { 232, 426, 135, 18 }, 10, muted);
    text(g, "LEVEL", { 419, 426, 135, 18 }, 10, muted);
    text(g, "INPUT PEAK", { w - 364, 426, 110, 18 }, 10, muted);
    text(g, "SUGGESTED", { w - 254, 426, 108, 18 }, 10, muted, false, juce::Justification::centred);

    const int bottom = getHeight() - 118;
    const int col = (w - 64) / 3;
    for (int i = 0; i < 3; ++i)
    {
        g.setColour(panel); g.fillRoundedRectangle(float(32 + i * col), float(bottom), float(col - 10), 88, 8);
    }
    text(g, "BALANCE AMOUNT", { 48, bottom + 10, 210, 22 }, 11, muted, true);
    text(g, "KICK  >  808 SPACE", { 48 + col, bottom + 10, 210, 22 }, 11, muted, true);
    text(g, "OUTPUT", { 48 + 2 * col, bottom + 10, 100, 22 }, 11, muted, true);
    text(g, "Applies to the next balance suggestion", { 48, bottom + 65, col - 38, 16 }, 10, muted);
    const auto duckText = owner.compare.load() ? juce::String("Comparing input - ducking bypassed")
        : owner.value("duck") < 0.05f ? juce::String("OFF - raise this slider to duck the 808")
        : "Ducking: " + juce::String(owner.duckReduction.load(), 1) + " dB";
    text(g, duckText, { 48 + col, bottom + 65, col - 38, 16 }, 10, owner.value("duck") < 0.05f ? muted : orange);
    text(g, "Guard reduction: " + juce::String(owner.reduction.load(), 1) + " dB", { 48 + 2 * col, bottom + 65, col - 38, 16 }, 10, muted);
    text(g, "POCKET AUDIO   /   " JucePlugin_VersionString, { 32, getHeight() - 25, 240, 20 }, 10, muted);
    text(g, "Separate inputs required  /  No added latency", { w - 475, getHeight() - 25, 440, 20 }, 10, muted, false, juce::Justification::centredRight);
}
void PocketBalanceEditor::resized()
{
    const int w = getWidth();
    analysisView.setBounds(w - 610, 43, 280, 28);
    waveforms.setBounds(24, 82, w - 48, 210);
    clashes.setBounds(waveforms.getBounds());
    listen.setBounds(40, 323, 178, 39); apply.setBounds(230, 323, 164, 39);
    undo.setBounds(406, 323, 138, 39);
    help.setBounds(w - 152, 392, 128, 28);
    pageOne.setBounds(164, 392, 86, 28); pageTwo.setBounds(258, 392, 86, 28);
    sourceNames.setBounds(358, 392, 148, 28);
    sourceSyncStatus.setBounds(280, getHeight() - 25, w - 780, 20);
    compare.setBounds(w - 454, 392, 132, 28); reset.setBounds(w - 308, 392, 138, 28);
    const int rowHeight = (getHeight() - 574) / 8;
    for (int i = 0; i < pocket::laneCount; ++i) lanes[size_t(i)]->setBounds(24, 448 + (i % 8) * rowHeight, w - 48, rowHeight - 3);
    const int bottom = getHeight() - 118, col = (w - 64) / 3;
    amount.setBounds(48, bottom + 34, col - 42, 29);
    duck.setBounds(48 + col, bottom + 34, col - 42, 29);
    output.setBounds(48 + 2 * col, bottom + 34, col - 42, 29);
    guard.setBounds(32 + 3 * col - 140, bottom + 7, 113, 24);
}
void PocketBalanceEditor::timerCallback()
{ refreshDisplay(); }
void PocketBalanceEditor::showSourceMapping()
{
    juce::Component::SafePointer<PocketBalanceEditor> editor(this);
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new SourceMappingPanel(owner,
        [editor](int track, int slot, const std::array<int, pocket::laneCount>& mapping)
        { if (editor != nullptr) editor->owner.setSourceMapping(track, slot, mapping); },
        [editor] { if (editor != nullptr) editor->owner.clearSourceMapping(); }));
    options.dialogTitle = "FL source names";
    options.dialogBackgroundColour = background;
    options.componentToCentreAround = this;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    options.launchAsync();
}
void PocketBalanceEditor::refreshDisplay()
{
    sourceSyncStatus.setText(owner.sourceSyncStatus(), juce::dontSendNotification);
    const int modelView = juce::jlimit(1, 2, int(owner.parameters.state.getProperty("analysisView", 1)));
    // Preserve a queued user selection when the saved model has not changed.
    if (modelView != observedAnalysisView)
    {
        observedAnalysisView = modelView;
        analysisView.setSelectedId(modelView, juce::dontSendNotification);
        selectAnalysisView(modelView);
    }
    waveforms.refresh();
    clashes.refresh();
    const auto suggestions = owner.getSuggestions();
    for (auto& lane : lanes) lane->refresh(suggestions, owner.value("amount") * 0.01f);
    const bool learning = owner.isLearning();
    if (!owner.hasCapture() || std::abs(owner.value("amount") - appliedAmount) > 0.1f) balanceApplied = false;
    for (int i = 0; i < pocket::laneCount; ++i)
        if (std::abs(owner.value(owner.laneId(i, "role")) - appliedRoles[size_t(i)]) > 0.1f) balanceApplied = false;
    listen.setButtonText(learning ? "Cancel listening" : "Listen  /  8 seconds");
    listen.setEnabled(learning || !owner.compare.load());
    apply.setEnabled(!learning && suggestions.activeLanes >= 2);
    undo.setEnabled(owner.canUndoBalance());
    compare.setToggleState(owner.compare.load(), juce::dontSendNotification);
    if (learning) status = "Listening... " + juce::String(8.0f * (1.0f - owner.progress()), 1) + " seconds left. Keep your loop playing.";
    else if (owner.compare.load()) status = "Balance bypassed. All routed inputs still pass through.";
    else if (balanceApplied) status = "Balance applied. Adjust the faders to taste.";
    else if (owner.hasCapture()) status = suggestions.activeLanes >= 2
        ? juce::String(suggestions.activeLanes) + " drums measured. Review suggestions, then Apply balance."
        : "Need two active inputs. Check routing and listen again.";
    else status = "Route your drums. Play a loop. Press Listen.";
    repaint();
}
void PocketBalanceEditor::showRouting()
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
        "Route your drums into Pocket Balance",
        "FL STUDIO / VST3\n\n"
        "1. Put each drum on its own Mixer track. Create an empty Pocket bus and insert this plugin.\n\n"
        "2. Route only your first drum normally to the Pocket bus. It becomes input 01. Disable that drum's direct Master send.\n\n"
        "3. Send each remaining drum to the Pocket bus using Sidechain to this track. Disable each drum's direct Master send.\n\n"
        "4. Open the plugin Wrapper settings > Processing > Connections. Map each sidechain source to a different input, 02 through 16."
        " Turn off Process inactive inputs and outputs. Disable EVERY unused input with its activation circle; --- alone does not silence it in FL."
        " Input 01 stays enabled and must contain only the first drum.\n\n"
        "5. Keep the Pocket bus routed to Master. Check that each drum lights up just one input meter. Name the lanes and choose roles.\n\n"
        "6. Play a representative loop. Press Listen, review the suggested levels, then Apply balance. Adjust to taste.\n\n"
        "FREQUENCY CLASHES: Select Frequency clashes above the analysis panel. Listen measures simultaneous band overlap and relative levels. "
        "Select a finding and read the named source to reduce. Preview correction temporarily applies up to 3 dB of dynamic band reduction. "
        "Apply correction keeps the preview. One correction can be active; applying another replaces it. Undo correction restores the previous setting. "
        "Disable correction turns it off and is also undoable. Closing the editor or switching views stops a preview; applied corrections remain. "
        "Changed levels, roles, mute, solo, ducking, or input connections require a fresh Listen. Similar-level findings are informational. "
        "Shared frequencies can be intentional. This analysis reports possible masking, not a guaranteed problem or cure. Compare by ear.\n\n"
        "BYPASS: Use Bypass balance inside this plugin to keep all inputs audible. FL's power switch skips the multi-input mixer and may leave only input 01 audible.\n\n"
        "NAMES: Enable the Pocket Balance Source Sync controller script in FL MIDI settings. Click FL source names and match each input to its Mixer track. "
        "Mixer names then update automatically. These updates preserve roles, Auto preferences, and audio settings. "
        "Re-link after changing Wrapper connections or copying a plugin preset. Other hosts keep manual source names. "
        "Double-click a Source label for manual naming. Auto infers a role only when you manually edit a label or click Auto.\n\n"
        "OTHER HOSTS\n"
        "Route separate mono or stereo sources to this effect's input buses. Your host must support multiple auxiliary inputs."
        " A mixed stereo drum loop cannot be separated by this plugin.\n\n"
        "Use more instances for more than 16 sources. Each instance balances its own group.\n\n"
        "LICENSE AND SOURCE\n"
        "Copyright 2026 Pocket Balance contributors. Licensed under AGPL-3.0-only. "
        "This program comes with no warranty, including no implied warranty of merchantability or fitness for a particular purpose.\n"
        "Source: https://github.com/dylanmccavitt/pocket-balance", "Got it", this);
}
