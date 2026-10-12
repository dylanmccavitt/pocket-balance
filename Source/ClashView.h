// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include "PluginProcessor.h"
#include <cmath>

// The message thread owns selection and display state. Audio publishes the report.
class ClashView : public juce::Component
{
public:
    explicit ClashView(PocketBalanceProcessor& p) : owner(p)
    {
        setName("frequency_clashes"); setTitle("Frequency clash analysis");
        finding.setName("clash_finding");
        finding.setTextWhenNothingSelected("Play a loop, then press Listen below");
        finding.setTooltip("At most eight possible masking findings from the latest Listen capture. Shared frequencies can be intentional.");
        finding.onChange = [this]
        {
            const int selected = finding.getSelectedId();
            if (selected == observedFinding) return;
            observedFinding = selected;
            owner.stopClashPreview();
            refresh();
        };
        preview.setName("clash_preview"); preview.setClickingTogglesState(true);
        preview.setTooltip("Temporarily reduce the named source in this frequency band when the protected source overlaps. Switching views or closing the editor stops the preview.");
        preview.onClick = [this]
        {
            if (preview.getToggleState()) owner.previewClash(finding.getSelectedId() - 1, float(depth.getValue()));
            else owner.stopClashPreview();
            refresh();
        };
        depth.setName("clash_depth"); depth.setRange(0.0, 3.0, 0.1);
        depth.setSliderStyle(juce::Slider::LinearHorizontal);
        depth.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 26);
        depth.setTextValueSuffix(" dB"); depth.setDoubleClickReturnValue(true, 2.0);
        depth.setValue(2.0, juce::dontSendNotification);
        depth.setTooltip("Maximum band reduction, from 0 to 3 dB. Changing this updates an active preview; an applied correction changes only after Apply.");
        depth.onValueChange = [this]
        {
            if (owner.isClashPreviewing()) owner.previewClash(finding.getSelectedId() - 1, float(depth.getValue()));
            refresh();
        };
        apply.setName("clash_apply"); undo.setName("clash_undo"); disable.setName("clash_disable");
        apply.setTooltip("Keep this preview as the one active correction. This replaces the previous correction; Undo restores it.");
        undo.setTooltip("Restore the previous applied correction. This is separate from Undo levels.");
        disable.setTooltip("Turn off the applied frequency correction. Undo can restore it.");
        apply.onClick = [this] { owner.applyClash(); refresh(); };
        undo.onClick = [this] { owner.undoClash(); refresh(); };
        disable.onClick = [this] { owner.disableClash(); refresh(); };
        configureLabel(state, "clash_status", 11, juce::Colour(0xffa3b1a7));
        configureLabel(evidence, "clash_evidence", 12, juce::Colour(0xffedf1e8));
        configureLabel(action, "clash_action", 12, juce::Colour(0xffc3ec85));
        configureLabel(active, "clash_active", 11, juce::Colour(0xffefb278));
        configureLabel(caution, "clash_caution", 11, juce::Colour(0xffa3b1a7));
        caution.setText("Possible masking only. Shared frequencies can be intentional; correction may not improve the mix.", juce::dontSendNotification);
        for (auto* c : std::initializer_list<juce::Component*> { &finding, &preview, &depth, &apply, &undo, &disable,
                                                              &state, &evidence, &action, &active, &caution })
            addAndMakeVisible(c);
        refresh();
    }
    ~ClashView() override { owner.stopClashPreview(); }

    void refresh()
    {
        const auto next = owner.getClashReport();
        const int nextCount = juce::jlimit(0, pocket::maxClashFindings, next.count);
        if (!sameReport(report, next) || nextCount != finding.getNumItems())
        {
            owner.stopClashPreview();
            report = next;
            finding.clear(juce::dontSendNotification);
            for (int i = 0; i < nextCount; ++i) finding.addItem(findingCaption(i), i + 1);
            observedFinding = nextCount == 0 ? 0 : juce::jlimit(1, nextCount, observedFinding);
            finding.setSelectedId(observedFinding, juce::dontSendNotification);
        }
        else
        {
            // Names can change without a new capture. Do not reset a pending selection.
            for (int i = 0; i < nextCount; ++i)
            {
                const auto caption = findingCaption(i);
                if (finding.getItemText(i) != caption) finding.changeItemText(i + 1, caption);
            }
        }

        const bool learning = owner.isLearning(), comparing = owner.compare.load();
        const bool fresh = owner.hasFreshClashCapture();
        const bool available = !learning && !comparing && fresh;
        const auto* selected = selectedFinding();
        const bool directional = selected != nullptr && validLane(selected->reduceLane) && validLane(selected->protectLane)
                                 && validBand(selected->band) && selected->reduceLane != selected->protectLane;
        // A new selection may still be waiting for ComboBox's asynchronous callback.
        // Never present an old preview as the correction for that new finding.
        if (!available || !directional || finding.getSelectedId() != observedFinding) owner.stopClashPreview();
        const bool previewing = owner.isClashPreviewing();
        preview.setToggleState(previewing, juce::dontSendNotification);
        preview.setButtonText(previewing ? "Stop preview" : "Preview correction");
        finding.setEnabled(!learning && !comparing && nextCount > 0);
        preview.setEnabled(available && directional);
        depth.setEnabled(available && directional);
        apply.setEnabled(available && directional && previewing);
        undo.setEnabled(!learning && !comparing && owner.canUndoClash());
        const auto applied = owner.appliedClash();
        disable.setEnabled(!learning && !comparing && (applied.enabled || previewing));

        if (learning) setText(state, "Listening for simultaneous frequency overlap...");
        else if (comparing) setText(state, "Balance bypassed. Corrections are bypassed.");
        else if (!owner.hasCapture()) setText(state, "Play a representative loop, then press Listen.");
        else if (!fresh) setText(state, "Results are stale. Press Listen again after changes.");
        else setText(state, juce::String(nextCount) + (nextCount == 1 ? " possible masking finding" : " possible masking findings"));

        if (selected != nullptr)
        {
            juce::String difference;
            if (std::abs(selected->levelDifferenceDb) < 0.05f) difference = "Similar band levels";
            else
            {
                const int louder = selected->levelDifferenceDb > 0 ? selected->first : selected->second;
                difference = shortLaneName(louder) + " is " + juce::String(std::abs(selected->levelDifferenceDb), 1) + " dB louder in this band";
            }
            setText(evidence, "Together: " + juce::String(selected->overlapSeconds, 2) + " s  /  "
                    + juce::String(selected->episodes) + " overlap episodes"
                    + (selected->sustained ? " (sustained)" : "") + "  /  " + difference);
            setText(action, directional
                ? "Preview target: reduce " + shortLaneName(selected->reduceLane) + " by up to " + juce::String(depth.getValue(), 1)
                    + " dB at " + bandName(selected->band) + " when " + shortLaneName(selected->protectLane) + " overlaps."
                : "Informational finding: no clear louder source. No automatic correction target.");
        }
        else
        {
            setText(evidence, fresh ? "No reportable simultaneous overlap in this capture." : "Each drum needs its own routed input for this analysis.");
            setText(action, "Listen measures overlap and relative band levels. It does not judge whether your mix sounds good.");
        }
        if (previewing)
            setText(active, "PREVIEW ONLY  /  Current reduction: " + juce::String(owner.clashReduction.load(), 1)
                           + " dB. Apply keeps this correction; stopping preview restores the applied setting.");
        else if (applied.enabled)
            setText(active, "APPLIED  /  Reduce " + shortLaneName(applied.source) + " at " + bandName(applied.band)
                    + " when " + shortLaneName(applied.trigger) + " overlaps  /  Up to " + juce::String(applied.depthDb, 1)
                    + " dB  /  Current reduction: " + juce::String(owner.clashReduction.load(), 1) + " dB");
        else setText(active, "No frequency correction applied. One correction can be active at a time.");
        repaint();
    }

    void resized() override
    {
        const int width = getWidth();
        state.setBounds(210, 10, width - 226, 25);
        finding.setBounds(16, 40, width - 32, 28);
        evidence.setBounds(16, 73, width - 32, 20);
        action.setBounds(16, 96, width - 32, 20);
        preview.setBounds(16, 122, 150, 30);
        depth.setBounds(221, 122, 166, 30);
        apply.setBounds(401, 122, 147, 30);
        undo.setBounds(560, 122, 145, 30);
        disable.setBounds(717, 122, 154, 30);
        active.setBounds(16, 158, width - 32, 20);
        caution.setBounds(16, 183, width - 32, 18);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff0c1210)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 10);
        g.setColour(juce::Colour(0xff29372f)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10, 1);
        g.setColour(juce::Colour(0xffc3ec85));
        g.setFont(juce::Font(juce::FontOptions(12, juce::Font::bold)));
        g.drawText("FREQUENCY CLASHES", { 16, 10, 194, 25 }, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffa3b1a7)); g.setFont(juce::Font(juce::FontOptions(11)));
        g.drawText("Depth", { 178, 122, 43, 30 }, juce::Justification::centredLeft);
    }

private:
    static bool validLane(int lane) { return lane >= 0 && lane < pocket::laneCount; }
    static bool validBand(int band) { return band >= 0 && band < pocket::clashBandCount; }
    static juce::String bandName(int band)
    {
        if (!validBand(band)) return "unknown band";
        return juce::String(int(pocket::clashBandLow[size_t(band)])) + "-"
               + juce::String(int(pocket::clashBandHigh[size_t(band)])) + " Hz";
    }
    juce::String shortLaneName(int lane) const
    { return validLane(lane) ? owner.laneName(lane).substring(0, 28) : juce::String("unknown input"); }
    juce::String findingCaption(int index) const
    {
        const auto& f = report.findings[size_t(index)];
        return juce::String(index + 1) + ". " + shortLaneName(f.first) + " + " + shortLaneName(f.second)
               + "  /  " + bandName(f.band) + (f.reduceLane < 0 ? "  /  Informational" : "  /  Possible masking");
    }
    const pocket::ClashFinding* selectedFinding() const
    {
        const int index = finding.getSelectedId() - 1;
        return index >= 0 && index < juce::jlimit(0, pocket::maxClashFindings, report.count) ? &report.findings[size_t(index)] : nullptr;
    }
    static bool sameReport(const pocket::ClashReport& a, const pocket::ClashReport& b)
    {
        if (a.count != b.count) return false;
        for (int i = 0; i < juce::jlimit(0, pocket::maxClashFindings, a.count); ++i)
        {
            const auto& x = a.findings[size_t(i)]; const auto& y = b.findings[size_t(i)];
            if (x.first != y.first || x.second != y.second || x.band != y.band || x.reduceLane != y.reduceLane
                || x.protectLane != y.protectLane || x.overlapSeconds != y.overlapSeconds
                || x.levelDifferenceDb != y.levelDifferenceDb || x.episodes != y.episodes || x.sustained != y.sustained
                || x.strength != y.strength) return false;
        }
        return true;
    }
    static void configureLabel(juce::Label& label, const char* name, float size, juce::Colour colour)
    {
        label.setName(name); label.setFont(juce::Font(juce::FontOptions(size)));
        label.setColour(juce::Label::textColourId, colour);
        label.setBorderSize(juce::BorderSize<int>(0));
    }
    static void setText(juce::Label& label, const juce::String& value)
    {
        if (label.getText() == value) return;
        label.setText(value, juce::dontSendNotification); label.setTooltip(value);
    }
    PocketBalanceProcessor& owner;
    pocket::ClashReport report {};
    juce::ComboBox finding;
    int observedFinding = 0;
    juce::TextButton preview { "Preview correction" }, apply { "Apply correction" }, undo { "Undo correction" }, disable { "Disable correction" };
    juce::Slider depth;
    juce::Label state, evidence, action, active, caution;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClashView)
};
