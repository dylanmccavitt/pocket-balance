// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "BalanceEngine.h"
#include "WaveformHistory.h"
#include "ClashEngine.h"
#include <atomic>

class PocketBalanceProcessor : public juce::AudioProcessor
{
public:
    PocketBalanceProcessor();
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Pocket Balance"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    static juce::String laneId(int lane, const char* field);
    juce::AudioProcessorValueTreeState parameters;
    // UI -> audio thread command. 1 starts a fresh capture, 2 cancels it.
    void startLearning();
    void cancelLearning();
    bool isLearning() const { return learning.load(); }
    bool hasCapture() const { return ready.load(std::memory_order_acquire); }
    float progress() const { return learnProgress.load(); }
    pocket::Suggestions getSuggestions() const;
    bool applyBalance();
    void undoBalance();
    bool canUndoBalance() const { return canUndo; }
    void resetLevels();
    void setParameterValue(const juce::String&, float);
    float value(const juce::String& id) const { return parameters.getRawParameterValue(id)->load(); }
    juce::String laneName(int) const;
    void setLaneName(int, const juce::String&);
    std::array<std::atomic<float>, pocket::laneCount> meters {};
    std::array<std::atomic<bool>, pocket::laneCount> connected {};
    std::atomic<float> outputPeak { 0 }, reduction { 0 }, duckReduction { 0 };
    std::atomic<bool> compare { false };
    pocket::ClashReport getClashReport() const;
    bool hasFreshClashCapture() const;
    bool previewClash(int findingIndex, float depthDb);
    void stopClashPreview() { previewConfig.store(0); }
    bool isClashPreviewing() const { return previewConfig.load() != 0; }
    bool applyClash();
    void undoClash();
    bool canUndoClash() const { return clashUndoReady.load(); }
    void disableClash();
    pocket::ClashConfig appliedClash() const;
    std::atomic<float> clashReduction { 0 };
    // One message-thread consumer. The scope is off while no editor needs it.
    void setScopeActive(bool active) noexcept { scope.setActive(active); }
    bool popScopeFrame(pocket::ScopeFrame& frame) noexcept { return scope.pop(frame); }

private:
    static BusesProperties makeBuses();
    static std::uint32_t packClash(const pocket::ClashConfig&);
    static pocket::ClashConfig unpackClash(std::uint32_t);
    void setClashParameters(const pocket::ClashConfig&);
    bool readAppliedClash(pocket::ClashConfig&) const;
    std::atomic<unsigned> clashWriters { 0 }, clashGeneration { 0 }, reportGeneration { 0 };
    pocket::ClashConfig audioAppliedClash;
    static constexpr int captureSettingCount = pocket::laneCount * 5 + 1;
    std::array<float, captureSettingCount> currentClashSettings() const;
    void publishClashes(const pocket::ClashReport&);
    struct AtomicClashFinding
    {
        std::atomic<int> first{-1}, second{-1}, band{0}, reduceLane{-1}, protectLane{-1}, episodes{0};
        std::atomic<float> overlapSeconds{0}, levelDifferenceDb{0}, strength{0};
        std::atomic<bool> sustained{false};
    };
    std::array<AtomicClashFinding, pocket::maxClashFindings> clashFindings;
    std::atomic<int> clashFindingCount { 0 };
    std::array<std::atomic<float>, captureSettingCount> capturedClashSettings {};
    std::array<float, captureSettingCount> learningClashSettings {};
    std::atomic<bool> clashCaptureStable { false };
    bool learningClashStable = false;
    pocket::ClashAnalyser clashAnalyser;
    pocket::ClashCorrection clashCorrection;
    std::atomic<std::uint32_t> previewConfig { 0 };
    std::atomic<bool> clashUndoReady { false };
    pocket::ClashConfig undoClashConfig;
    std::array<std::atomic<float>*, 5> clashParameters {};
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
    void process(juce::AudioBuffer<float>&, bool bypass);
    struct LaneParameters { std::atomic<float>* gain; std::atomic<float>* role;
                            std::atomic<float>* mute; std::atomic<float>* solo; };
    std::array<LaneParameters, pocket::laneCount> laneParameters;
    std::atomic<float>* outputParameter = nullptr;
    std::atomic<float>* duckParameter = nullptr;
    std::atomic<float>* guardParameter = nullptr;
    std::array<pocket::Analyser, pocket::laneCount> analysers;
    std::array<pocket::Smoother, pocket::laneCount> gainSmoothers;
    pocket::Smoother outputSmoother, duckSmoother;
    pocket::DuckEnvelope duck;
    pocket::PeakGuard guard;
    pocket::WaveformHistory scope;
    std::array<std::atomic<float>, pocket::laneCount> capturedRms {}, capturedPeak {};
    std::array<std::atomic<bool>, pocket::laneCount> capturedValid {};
    std::atomic<int> learnCommand { 0 };
    std::atomic<bool> learning { false }, ready { false };
    std::atomic<float> learnProgress { 0 };
    std::int64_t captureSamples = 0, captureLength = 384000;
    float meterDecay = 0.99f;
    std::array<float, pocket::laneCount> undoGains {};
    std::atomic<bool> canUndo { false };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PocketBalanceProcessor)
};
