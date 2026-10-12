// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "WaveformView.h"
#include "ClashView.h"

class PocketLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PocketLookAndFeel();
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawLinearSlider(juce::Graphics&, int, int, int, int, float, float, float,
                          juce::Slider::SliderStyle, juce::Slider&) override;
};

class PocketBalanceEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PocketBalanceEditor(PocketBalanceProcessor&);
    ~PocketBalanceEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void refreshDisplay();
private:
    struct Lane : public juce::Component
    {
        Lane(PocketBalanceProcessor&, int);
        void paint(juce::Graphics&) override;
        void resized() override;
        void refresh(const pocket::Suggestions&, float amount);
        void updateIdentity();
        PocketBalanceProcessor& owner;
        int index;
        int observedRole = 0;
        juce::Label name;
        juce::ComboBox role;
        juce::Slider gain;
        juce::TextButton mute { "M" }, solo { "S" };
        juce::TextButton autoRole { "Auto" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment, soloAttachment;
        juce::String suggestionText = "--";
    };
    void timerCallback() override;
    void showRouting();
    void selectPage(int);
    void selectAnalysisView(int);
    PocketBalanceProcessor& owner;
    PocketLookAndFeel look;
    juce::TooltipWindow tooltip { this, 600 };
    WaveformView waveforms;
    ClashView clashes;
    juce::ComboBox analysisView;
    int observedAnalysisView = 1;
    std::array<std::unique_ptr<Lane>, pocket::laneCount> lanes;
    juce::TextButton listen { "Listen  /  8 seconds" }, apply { "Apply balance" }, undo { "Undo levels" };
    juce::TextButton compare { "Bypass balance" }, reset { "Reset levels" }, help { "Routing guide" };
    juce::TextButton pageOne { "01 - 08" }, pageTwo { "09 - 16" }, guard { "Peak guard" };
    juce::Slider amount, duck, output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amountAttachment, duckAttachment, outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> guardAttachment;
    juce::String status = "Route your drums. Play a loop. Press Listen.";
    int page = 0;
    bool balanceApplied = false;
    float appliedAmount = 0;
    std::array<float, pocket::laneCount> appliedRoles {};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PocketBalanceEditor)
};
