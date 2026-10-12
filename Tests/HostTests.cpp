// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void near(float a, float b, float tolerance, const char* message)
{ require(std::abs(a - b) <= tolerance, message); }

void fill(juce::AudioBuffer<float>& buffer, const std::array<float, 16>& amplitudes, int start, double rate)
{
    buffer.clear();
    for (int i = 0; i < 16; ++i)
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float sample = amplitudes[size_t(i)] * std::sin(float(start + n) * 6.28318530718f * (60.0f + float(i) * 30.0f) / float(rate));
            buffer.setSample(i * 2, n, sample); buffer.setSample(i * 2 + 1, n, -sample * 0.8f);
        }
}

void testScopeQueue()
{
    pocket::WaveformHistory history;
    history.prepare(500); // One sample per frame makes overflow boundaries exact.
    history.setActive(true);
    std::array<float, pocket::scopeWaveCount> left {}, right {};
    constexpr int overflow = 73;
    for (std::uint32_t n = 1; n <= pocket::WaveformHistory::capacity + overflow; ++n)
    {
        left[0] = float(n); right[0] = -float(n);
        history.pushSample(left, right, float(n));
    }
    pocket::ScopeFrame frame;
    int received = 0;
    while (history.pop(frame))
    {
        ++received;
        near(frame.waves[0].min, -float(received), 0, "Overflow must not overwrite queued negative samples");
        near(frame.waves[0].max, float(received), 0, "Overflow must not overwrite queued positive samples");
        require(std::abs(frame.seconds - double(received) / 500.0) < 1.0e-8,
                "Queue preserves chronological sample timestamps");
    }
    require(received == int(pocket::WaveformHistory::capacity), "Scope queue holds 4096 complete frames");
    history.pushSample(left, right, 0);
    require(history.pop(frame), "Queue accepts new frames after overflow is drained");
    require(std::abs(frame.seconds - double(received + overflow + 1) / 500.0) < 1.0e-8,
            "Dropped frames remain visible as a timestamp gap");
    history.setActive(false);
    require(!history.beginBlock(), "Scope can be disabled without resetting producer cursors");
    const double beforePause = frame.seconds;
    history.skipSamples(500);
    history.setActive(true);
    require(history.beginBlock(), "Scope resumes capture");
    history.pushSample(left, right, 0);
    require(history.pop(frame) && frame.seconds > beforePause + 1.0,
            "Scope clock advances while its view is inactive");
    const double beforeRateChange = frame.seconds;
    history.prepare(1000);
    history.pushSample(left, right, 0);
    require(!history.pop(frame), "Rate change adapts the scope bucket size");
    history.pushSample(left, right, 0);
    require(history.pop(frame) && frame.seconds > beforeRateChange,
            "Rate changes retain monotonic timestamps without a cross-thread queue reset");

    // Stress publication and wraparound with a real concurrent UI consumer.
    pocket::WaveformHistory concurrent;
    concurrent.prepare(500); concurrent.setActive(true);
    std::atomic<bool> finished { false };
    std::thread producer([&]
    {
        std::array<float, pocket::scopeWaveCount> a {}, b {};
        for (int n = 1; n <= 30000; ++n)
        {
            a.fill(float(n)); b.fill(-float(n));
            concurrent.pushSample(a, b, float(n));
            if (n % 64 == 0) std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });
    int last = 0, count = 0;
    const auto check = [&]
    {
        const int sequence = int(frame.duckDb);
        require(sequence > last, "Concurrent frames cannot repeat or arrive out of order");
        for (const auto& range : frame.waves)
        {
            near(range.min, -float(sequence), 0, "Consumer sees a fully published negative range");
            near(range.max, float(sequence), 0, "Consumer sees a fully published positive range");
        }
        require(std::abs(frame.seconds - double(sequence) / 500.0) < 1.0e-7,
                "Concurrent frame payload and timestamp belong to the same sample");
        last = sequence; ++count;
    };
    while (!finished.load(std::memory_order_acquire))
    {
        if (concurrent.pop(frame)) check();
        else std::this_thread::yield();
    }
    producer.join();
    while (concurrent.pop(frame)) check();
    require(count > 0, "Concurrent scope producer delivers frames");
}

void testScopeTimingAndStereo()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        PocketBalanceProcessor p;
        p.setParameterValue("output", 0); p.setParameterValue("guard", 0);
        p.setParameterValue(p.laneId(0, "gain"), -6);
        p.prepareToPlay(rate, 128); p.setScopeActive(true);
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
        constexpr std::array<int, 5> sizes { 1, 37, 128, 511, 2048 };
        int cursor = 0, frames = 0;
        double previous = 0;
        const float amplitude = 0.25f * pocket::gainFromDb(-6);
        for (int block = 0; cursor < int(rate); ++block)
        {
            const int samples = std::min(int(rate) - cursor, sizes[size_t(block) % sizes.size()]);
            buffer.setSize(32, samples); buffer.clear();
            for (int n = 0; n < samples; ++n)
            {
                buffer.setSample(0, n, 0.25f); buffer.setSample(1, n, -0.25f);
            }
            p.processBlock(buffer, midi); cursor += samples;
            pocket::ScopeFrame frame;
            while (p.popScopeFrame(frame))
            {
                ++frames;
                near(frame.waves[0].min, -amplitude, 0.000001f, "Scope preserves negative antiphase stereo after the fader");
                near(frame.waves[0].max, amplitude, 0.000001f, "Scope preserves positive antiphase stereo after the fader");
                near(frame.waves[pocket::scopeOutput].min, -amplitude, 0.000001f, "Output scope follows actual stereo audio");
                near(frame.waves[pocket::scopeKick].max, amplitude, 0.000001f, "Kick scope sums role-assigned signals");
                near(frame.waves[15].max, 0, 0, "Silent lane waveform stays zero");
                require(frame.seconds > previous, "Scope time always moves forward");
                require(std::abs(frame.seconds - double(frames) / pocket::scopeFramesPerSecond) <= 1.0 / rate + 1.0e-8,
                        "Scope cadence remains 500 Hz across sample rates and variable blocks");
                previous = frame.seconds;
            }
        }
        require(frames == 500, "Every tested sample rate generates 500 scope frames per second");
        require(std::abs(previous - 1.0) < 1.0e-8, "One second of samples occupies one second of scope time");
    }
}

void testScopeProcessedSignals()
{
    PocketBalanceProcessor p;
    p.setParameterValue("output", -6); p.setParameterValue("guard", 0); p.setParameterValue("duck", 6);
    p.setParameterValue(p.laneId(1, "gain"), -6);
    p.setParameterValue(p.laneId(2, "mute"), 1);
    p.prepareToPlay(48000, 128); p.setScopeActive(true);
    juce::AudioBuffer<float> buffer(32, 480);
    juce::MidiBuffer midi;
    const auto fillConstant = [&]
    {
        buffer.clear();
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            buffer.setSample(0, n, 0.5f); buffer.setSample(1, n, -0.5f);
            buffer.setSample(2, n, 0.4f); buffer.setSample(3, n, -0.4f);
            buffer.setSample(4, n, 0.3f); buffer.setSample(5, n, -0.3f);
        }
    };
    pocket::ScopeFrame last;
    const auto processSteady = [&](bool bypass)
    {
        bool received = false;
        for (int block = 0; block < 40; ++block)
        {
            fillConstant();
            if (bypass) p.processBlockBypassed(buffer, midi); else p.processBlock(buffer, midi);
            while (p.popScopeFrame(last)) received = true;
        }
        require(received, "Processed audio produces scope frames");
    };
    processSteady(false);
    const float gain = pocket::gainFromDb(-6);
    near(last.waves[pocket::scopeBassBefore].max, 0.4f * gain, 0.00001f, "Bass-before scope includes the lane fader");
    near(last.waves[pocket::scopeBassAfter].max, 0.4f * gain * gain, 0.00002f, "Bass-after scope includes audible kick ducking");
    near(last.waves[1].max, last.waves[pocket::scopeBassAfter].max, 0, "Lane scope uses post-duck signal");
    near(last.waves[2].max, 0, 0.000001f, "Muted lane scope becomes silent");
    near(last.duckDb, 6, 0.001f, "Scope reports peak applied duck depth per frame");
    near(last.waves[pocket::scopeOutput].max, (0.5f + 0.4f * gain * gain) * gain, 0.00002f,
         "Output scope includes final trim");

    p.compare.store(true); processSteady(false);
    near(last.waves[pocket::scopeBassBefore].max, 0.4f, 0.00001f, "Compare scope uses unity lane gains");
    near(last.waves[pocket::scopeBassAfter].max, 0.4f, 0.00001f, "Compare removes ducking after its gain ramp");
    near(last.duckDb, 0, 0.00001f, "Compare displays no ducking after the ramp settles");
    near(last.waves[2].max, 0, 0.000001f, "Compare continues respecting mute");

    p.compare.store(false); processSteady(true);
    near(last.waves[1].max, 0.4f, 0, "Host bypass scope uses unity bass gain");
    near(last.waves[2].max, 0.3f, 0, "Host bypass scope includes otherwise muted sources");
    near(last.waves[pocket::scopeOutput].max, 1.2f, 0.000001f, "Host bypass scope reflects untrimmed, unguarded summing");
    near(last.duckDb, 0, 0, "Host bypass never reports unapplied internal ducking");

    p.setParameterValue("guard", 1); p.setParameterValue("output", 6);
    p.setParameterValue("duck", 0); p.setParameterValue(p.laneId(2, "mute"), 0);
    processSteady(false);
    require(last.waves[pocket::scopeOutput].max <= 0.891252f
            && last.waves[pocket::scopeOutput].min >= -0.891252f,
            "Output scope includes sample-peak guard attenuation");
    near(last.waves[pocket::scopeOutput].max, buffer.getSample(0, buffer.getNumSamples() - 1), 0.00001f,
         "Final scope sample matches the guarded output buffer");
}

void testScopeDoesNotChangeAudio()
{
    PocketBalanceProcessor reference, visualised;
    for (auto* p : { &reference, &visualised })
    {
        p->setParameterValue("duck", 4.5f);
        p->setParameterValue(p->laneId(1, "gain"), -3);
        p->prepareToPlay(48000, 128);
    }
    visualised.setScopeActive(true);
    std::array<float, 16> amplitudes;
    amplitudes.fill(0.15f);
    juce::AudioBuffer<float> a(32, 511), b(32, 511);
    juce::MidiBuffer midi;
    for (int block = 0; block < 50; ++block)
    {
        fill(a, amplitudes, block * 511, 48000); b.makeCopyOf(a);
        reference.processBlock(a, midi); visualised.processBlock(b, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int n = 0; n < a.getNumSamples(); ++n)
                near(a.getSample(channel, n), b.getSample(channel, n), 0,
                     "Enabling telemetry does not change any output sample");
    }
    pocket::ScopeFrame frame;
    require(!reference.popScopeFrame(frame), "Hidden editor leaves telemetry inactive");
    require(visualised.popScopeFrame(frame), "Visible editor receives telemetry for identical audio");
}

juce::Component* findNamedComponent(juce::Component& parent, const juce::String& name)
{
    if (parent.getName() == name) return &parent;
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
        if (auto* found = findNamedComponent(*parent.getChildComponent(i), name)) return found;
    return nullptr;
}

void testSourceIdentityUiAndRecall()
{
    PocketBalanceProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* view = dynamic_cast<PocketBalanceEditor*>(editor.get());
    auto* name = dynamic_cast<juce::Label*>(findNamedComponent(*editor, "source_0"));
    auto* role = dynamic_cast<juce::ComboBox*>(findNamedComponent(*editor, "role_0"));
    auto* automatic = dynamic_cast<juce::TextButton*>(findNamedComponent(*editor, "auto_role_0"));
    require(view && name && role && automatic, "Source identity controls are exposed in the native editor");
    require(automatic->getToggleState(), "An unnamed drum lane initially infers roles from source labels");

    name->setText("808 clap", juce::sendNotificationSync);
    view->refreshDisplay();
    near(p.value(p.laneId(0, "role")), 3, 0, "Explicit clap identity takes priority over the 808 token");
    require(role->getSelectedId() == 4 && automatic->getToggleState()
            && bool(p.parameters.state.getProperty("autoRole0")),
            "Refreshing an inferred role preserves Auto in the UI and saved state");

    role->setSelectedId(1, juce::sendNotificationSync);
    near(p.value(p.laneId(0, "role")), 0, 0, "A manual role choice updates the audio parameter");
    require(!automatic->getToggleState() && !bool(p.parameters.state.getProperty("autoRole0")),
            "A manual role choice disables and persists Auto");
    name->setText("Closed hat", juce::sendNotificationSync);
    view->refreshDisplay();
    near(p.value(p.laneId(0, "role")), 0, 0, "Renaming a manually assigned lane preserves its role");
    automatic->setToggleState(true, juce::sendNotificationSync);
    view->refreshDisplay();
    near(p.value(p.laneId(0, "role")), 4, 0, "Enabling Auto infers the current source label immediately");
    name->setText("Velvet texture", juce::sendNotificationSync);
    view->refreshDisplay();
    near(p.value(p.laneId(0, "role")), 4, 0, "An unknown source label keeps the previously assigned role");
    require(automatic->getToggleState() && bool(p.parameters.state.getProperty("autoRole0")),
            "Editing an unknown label retains the Auto preference");

    // Saved roles may intentionally differ from the label. Restoring a project
    // while the editor is open must never reinterpret stale or recalled text.
    PocketBalanceProcessor preset;
    for (bool autoEnabled : { true, false })
    {
        preset.setLaneName(0, autoEnabled ? "Kick - Recalled source" : "808 - Recalled source");
        const float savedRole = autoEnabled ? 6.0f : 2.0f;
        preset.setParameterValue(preset.laneId(0, "role"), savedRole);
        preset.parameters.state.setProperty("autoRole0", autoEnabled, nullptr);
        juce::MemoryBlock state; preset.getStateInformation(state);
        p.setStateInformation(state.getData(), int(state.getSize()));
        view->refreshDisplay();
        view->refreshDisplay();
        near(p.value(p.laneId(0, "role")), savedRole, 0,
             "Open-editor state recall preserves the saved role without triggering label inference");
        require(name->getText() == preset.laneName(0) && role->getSelectedId() == int(savedRole) + 1,
                "Open-editor state recall refreshes the displayed source and role");
        require(automatic->getToggleState() == autoEnabled
                && bool(p.parameters.state.getProperty("autoRole0")) == autoEnabled,
                "State recall preserves both true and false Auto values in the UI and saved state");
    }
}

void testAsyncDropdownRefresh()
{
    PocketBalanceProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* view = dynamic_cast<PocketBalanceEditor*>(editor.get());
    require(view != nullptr, "Async selection regression uses the native editor");
    struct Preference { const char* component; const char* property; int initial; int selected; };
    constexpr std::array<Preference, 3> preferences {{
        { "scope_view", "scopeView", 1, 2 },
        { "scope_time", "scopeTime", 2, 4 },
        { "scope_zoom", "scopeZoom", 2, 8 }
    }};
    for (const auto& preference : preferences)
    {
        auto* control = dynamic_cast<juce::ComboBox*>(findNamedComponent(*editor, preference.component));
        require(control != nullptr, "Waveform preference control exists");
        control->setSelectedId(preference.selected, juce::sendNotificationAsync);
        view->refreshDisplay();
        view->refreshDisplay();
        require(control->getSelectedId() == preference.selected,
                "A timer refresh cannot erase a waveform selection before its asynchronous callback");
        require(int(p.parameters.state.getProperty(preference.property, preference.initial)) == preference.initial,
                "The regression exercises a selection that has not yet reached saved preferences");
        // Force the delayed handler after the refresh ordering above. This test
        // executable does not run a native message loop; no timer timing is involved.
        control->onChange();
        view->refreshDisplay();
        require(control->getSelectedId() == preference.selected
                && int(p.parameters.state.getProperty(preference.property)) == preference.selected,
                "The delayed waveform callback persists the selected preference");
        p.parameters.state.setProperty(preference.property, preference.initial, nullptr);
        view->refreshDisplay();
        require(control->getSelectedId() == preference.initial,
                "A changed model preference still refreshes the waveform control");
        control->onChange();
        require(int(p.parameters.state.getProperty(preference.property)) == preference.initial,
                "A callback superseded by model refresh cannot undo recalled preferences");
    }

    auto* role = dynamic_cast<juce::ComboBox*>(findNamedComponent(*editor, "role_0"));
    auto* automatic = dynamic_cast<juce::TextButton*>(findNamedComponent(*editor, "auto_role_0"));
    require(role && automatic, "Async role regression finds native identity controls");
    role->setSelectedId(4, juce::sendNotificationAsync);
    view->refreshDisplay();
    view->refreshDisplay();
    require(role->getSelectedId() == 4 && automatic->getToggleState(),
            "Refreshing an unchanged role parameter preserves a pending manual choice");
    near(p.value(p.laneId(0, "role")), 0, 0, "The role callback is still pending before forced delivery");
    role->onChange();
    view->refreshDisplay();
    near(p.value(p.laneId(0, "role")), 3, 0, "The delayed manual role callback applies the chosen role");
    require(!automatic->getToggleState() && !bool(p.parameters.state.getProperty("autoRole0")),
            "The delayed manual role callback disables Auto");

    role->setSelectedId(5, juce::sendNotificationAsync);
    p.setParameterValue(p.laneId(0, "role"), 2);
    p.parameters.state.setProperty("autoRole0", true, nullptr);
    view->refreshDisplay();
    role->onChange();
    require(role->getSelectedId() == 3 && automatic->getToggleState()
            && bool(p.parameters.state.getProperty("autoRole0")),
            "A pending role callback superseded by host state cannot disable recalled Auto");
    near(p.value(p.laneId(0, "role")), 2, 0, "A host role update supersedes the stale pending manual callback");
}

void testMixedRoutingAndRates()
{
    PocketBalanceProcessor p;
    auto layout = p.getBusesLayout();
    for (auto& bus : layout.inputBuses) bus = juce::AudioChannelSet::disabled();
    layout.inputBuses.getReference(0) = juce::AudioChannelSet::mono();
    layout.inputBuses.getReference(3) = juce::AudioChannelSet::mono();
    layout.inputBuses.getReference(15) = juce::AudioChannelSet::stereo();
    require(p.setBusesLayout(layout), "Apply a mixed mono, stereo, and disabled input layout");
    require(p.getTotalNumInputChannels() == 4, "Disabled input buses do not occupy channels");
    p.setParameterValue("output", 0); p.setParameterValue("guard", 0);
    p.setParameterValue(p.laneId(4, "solo"), 1);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer;
    constexpr std::array<int, 5> sizes { 1, 37, 128, 511, 2048 };
    const auto fillMixed = [&]
    {
        auto main = p.getBusBuffer(buffer, true, 0);
        auto mono = p.getBusBuffer(buffer, true, 3);
        auto stereo = p.getBusBuffer(buffer, true, 15);
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            main.setSample(0, n, 0.1f); mono.setSample(0, n, -0.025f);
            stereo.setSample(0, n, 0.2f); stereo.setSample(1, n, -0.15f);
        }
    };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        p.prepareToPlay(rate, 128);
        for (int size : sizes)
        {
            buffer.setSize(4, size); fillMixed(); p.processBlock(buffer, midi);
            for (int n = 0; n < size; ++n)
            {
                near(buffer.getSample(0, n), 0.275f, 0.00001f, "Mixed layout sums left without disabled solo silencing it");
                near(buffer.getSample(1, n), -0.075f, 0.00001f, "Mono sources reach both output channels without alias corruption");
            }
        }
        require(p.connected[0] && p.connected[3] && p.connected[15] && !p.connected[4],
                "Connection meters follow the actual enabled buses");
        fillMixed(); p.processBlockBypassed(buffer, midi);
        near(buffer.getSample(0, 0), 0.275f, 0.00001f, "Bypass sums the actual mixed bus layout");
        near(buffer.getSample(1, 0), -0.075f, 0.00001f, "Bypass duplicates mono inputs to right");

        p.startLearning();
        const int captureSamples = int(rate * 8);
        int cursor = 0;
        for (int block = 0; cursor < captureSamples; ++block)
        {
            const int count = std::min(captureSamples - cursor, sizes[size_t(block) % sizes.size()]);
            buffer.setSize(4, count); fillMixed(); p.processBlock(buffer, midi); cursor += count;
            if (cursor < captureSamples)
                require(p.isLearning() && !p.hasCapture(), "Capture cannot finish before eight seconds at the selected sample rate");
        }
        require(p.hasCapture() && !p.isLearning(), "Capture ends at eight seconds at every tested sample rate");
        require(p.getSuggestions().activeLanes == 3, "Capture includes enabled mono sources and excludes disabled sources");
        near(p.progress(), 1, 0.00001f, "Completed capture progress reaches one");
    }
}

void fillClashLoop(juce::AudioBuffer<float>& buffer, int cursor, double rate)
{
    buffer.clear();
    for (int n = 0; n < buffer.getNumSamples(); ++n)
    {
        const double t = double(cursor + n) / rate;
        const float burst = std::fmod(t, 0.5) < 0.3 ? 1.0f : 0.0f;
        const float tone = burst * float(std::sin(t * 6.283185307179586 * 110.0));
        buffer.setSample(0, n, tone * 0.4f); buffer.setSample(1, n, -tone * 0.4f);
        buffer.setSample(2, n, tone * 0.12f); buffer.setSample(3, n, -tone * 0.12f);
    }
}
void captureClashLoop(PocketBalanceProcessor& p, double rate = 48000)
{
    p.startLearning(); juce::MidiBuffer midi; juce::AudioBuffer<float> buffer;
    int cursor = 0;
    while (cursor < int(rate * 8))
    {
        const int count = std::min(int(rate * 8) - cursor, cursor % 2 ? 511 : 127);
        buffer.setSize(32, count); fillClashLoop(buffer, cursor, rate); p.processBlock(buffer, midi); cursor += count;
    }
}
void testClashWorkflow()
{
    PocketBalanceProcessor p;
    require(p.getParameters().size() == 75, "Source transport appends to the 73 existing parameters");
    auto* originalLast = dynamic_cast<juce::AudioProcessorParameterWithID*>(p.getParameters()[67]);
    require(originalLast && originalLast->paramID == "guard", "Existing parameter order remains unchanged");
    p.setParameterValue("guard", 0); p.setParameterValue("output", 0); p.prepareToPlay(48000, 511);
    require(!p.appliedClash().enabled && !p.previewClash(0, 2), "Clash correction starts disabled and needs analysis");
    captureClashLoop(p);
    require(p.hasFreshClashCapture() && p.getClashReport().count > 0, "Simultaneous same-band drums yield fresh evidence");
    const auto finding = p.getClashReport().findings[0];
    require(finding.reduceLane == 0 && finding.protectLane == 1, "Correction reduces louder input to protect quieter input");
    require(p.previewClash(0, 2), "Preview starts for a measured directional finding");
    require(p.isClashPreviewing() && !p.appliedClash().enabled, "Preview does not change saved correction parameters");
    juce::MemoryBlock previewState; p.getStateInformation(previewState);
    PocketBalanceProcessor previewRestore; previewRestore.setStateInformation(previewState.getData(), int(previewState.getSize()));
    require(!previewRestore.appliedClash().enabled && !previewRestore.isClashPreviewing(), "Preview is never serialized");
    p.stopClashPreview(); require(!p.isClashPreviewing(), "Stop preview restores saved processing");
    require(p.previewClash(0, 2) && p.applyClash(), "Apply commits the preview");
    require(p.appliedClash().enabled && !p.isClashPreviewing() && p.canUndoClash(), "Applied correction is persistent and undoable");
    require(p.previewClash(0, 2) && p.applyClash(), "Applying the same correction is idempotent");
    p.undoClash(); require(!p.appliedClash().enabled && !p.canUndoClash(), "Repeated Apply preserves the original disabled state for Undo");
    require(p.previewClash(0, 2) && p.applyClash(), "Apply again after Undo");
    p.setLaneName(0, "Low tom");
    juce::MemoryBlock appliedState; p.getStateInformation(appliedState);
    PocketBalanceProcessor restored; restored.setStateInformation(appliedState.getData(), int(appliedState.getSize()));
    require(restored.appliedClash().enabled && restored.appliedClash().source == 0
        && restored.appliedClash().trigger == 1 && restored.appliedClash().band == finding.band,
        "Applied correction pair and band survive state recall");
    near(restored.appliedClash().depthDb, 2, 0.001f, "Applied depth survives recall");
    require(restored.laneName(0) == "Low tom" && !restored.hasCapture() && !restored.canUndoClash(),
        "Recall keeps names but clears stale capture and undo history");
    p.disableClash(); require(!p.appliedClash().enabled && p.canUndoClash(), "Disable correction can be undone");
    p.undoClash(); require(p.appliedClash().enabled, "Undo Disable restores applied correction");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        auto* view = dynamic_cast<PocketBalanceEditor*>(editor.get());
        auto* mode = dynamic_cast<juce::ComboBox*>(findNamedComponent(*editor, "analysis_view"));
        auto* previewButton = dynamic_cast<juce::TextButton*>(findNamedComponent(*editor, "clash_preview"));
        auto* applyButton = dynamic_cast<juce::TextButton*>(findNamedComponent(*editor, "clash_apply"));
        auto* selection = dynamic_cast<juce::ComboBox*>(findNamedComponent(*editor, "clash_finding"));
        require(view && mode && previewButton && applyButton && selection, "New clash controls are accessible by stable component names");
        mode->setSelectedId(2, juce::sendNotificationAsync); view->refreshDisplay();
        require(mode->getSelectedId() == 2, "Refresh preserves a pending analysis-view choice");
        mode->onChange(); view->refreshDisplay();
        require(int(p.parameters.state.getProperty("analysisView")) == 2 && previewButton->isEnabled(),
                "Frequency view and fresh measured preview are available");
        previewButton->setToggleState(true, juce::dontSendNotification); previewButton->onClick();
        require(p.isClashPreviewing() && applyButton->isEnabled(), "Preview UI enables Apply for the actual pending correction");
        mode->setSelectedId(1, juce::dontSendNotification); mode->onChange();
        require(!p.isClashPreviewing() && p.appliedClash().enabled, "Switching view cancels only the temporary preview");
        mode->setSelectedId(2, juce::dontSendNotification); mode->onChange();
        require(p.previewClash(0, 1), "Editor close preview test starts");
    }
    require(!p.isClashPreviewing() && p.appliedClash().enabled, "Editor closure ends preview but preserves applied correction");
    p.setParameterValue(p.laneId(0, "gain"), -2);
    require(!p.hasFreshClashCapture() && !p.previewClash(0, 2), "Changed gains require new frequency analysis");
    p.setParameterValue(p.laneId(0, "gain"), 0);
    p.setParameterValue(p.laneId(1, "mute"), 1);
    require(!p.hasFreshClashCapture(), "Changed mute invalidates captured overlap");
    p.setParameterValue(p.laneId(1, "mute"), 0);
    p.compare.store(true);
    require(!p.previewClash(0, 2), "Comparison prevents preview");
    juce::MidiBuffer midi; juce::AudioBuffer<float> buffer(32, 511);
    for (int pass = 0; pass < 32; ++pass) { fillClashLoop(buffer, pass * 511, 48000); p.processBlock(buffer, midi); }
    fillClashLoop(buffer, 0, 48000); const float expected = buffer.getSample(0, 100) + buffer.getSample(2, 100);
    p.processBlock(buffer, midi);
    near(buffer.getSample(0, 100), expected, 0.0001f, "Internal comparison bypasses frequency correction without dropping inputs");
    near(p.clashReduction.load(), 0, 0.001f, "Comparison reports no frequency reduction");
    p.compare.store(false); fillClashLoop(buffer, 0, 48000); p.processBlockBypassed(buffer, midi);
    near(buffer.getSample(0, 100), expected, 0.00001f, "Host bypass ignores correction and preserves auxiliary audio");
    // Simulate a 0.2 state: no new parameter nodes exist in its XML.
    auto oldTree = p.parameters.copyState();
    for (int i = oldTree.getNumChildren() - 1; i >= 0; --i)
        if (oldTree.getChild(i).getProperty("id").toString().startsWith("clash_")) oldTree.removeChild(i, nullptr);
    juce::MemoryBlock oldState; juce::AudioProcessor::copyXmlToBinary(*oldTree.createXml(), oldState);
    p.setStateInformation(oldState.getData(), int(oldState.getSize()));
    require(!p.appliedClash().enabled && !p.isClashPreviewing(), "Old state clears an already active correction");
    near(p.value("clash_trigger"), 1, 0, "Old state supplies safe default trigger");
    near(p.value("clash_depth"), 2, 0, "Old state supplies default depth");
    PocketBalanceProcessor fresh; fresh.setStateInformation(oldState.getData(), int(oldState.getSize()));
    require(!fresh.appliedClash().enabled, "Old state loads unchanged in a fresh 0.3 instance");
    captureClashLoop(p);
    require(p.previewClash(0, 2), "New capture refreshes evidence");
    p.startLearning(); require(!p.isClashPreviewing() && !p.hasFreshClashCapture(), "Starting capture clears preview and old evidence");
    p.cancelLearning(); fillClashLoop(buffer, 0, 48000); p.processBlock(buffer, midi);
    require(!p.hasCapture() && !p.isLearning(), "Cancellation clears analysis and preview");
    captureClashLoop(p);
    require(p.applyBalance() && !p.hasFreshClashCapture(), "Applying balance requires recapturing frequency evidence");
}

void testSourceSyncProbe()
{
    PocketBalanceProcessor p;
    auto* packet = dynamic_cast<juce::AudioProcessorParameterWithID*>(p.getParameters()[73]);
    auto* ack = dynamic_cast<juce::AudioProcessorParameterWithID*>(p.getParameters()[74]);
    require(packet && packet->paramID == "source_sync_packet" && ack && ack->paramID == "source_sync_ack",
            "Transport parameters append with stable IDs");
    require(!packet->isAutomatable() && !ack->isAutomatable()
        && ack->getCategory() == juce::AudioProcessorParameter::otherMeter,
        "Transport is not automatable and acknowledgement is a meter");
    const float value = float(0xabcdef) / 16777216.0f;
    packet->setValue(value);
    near(ack->getValue(), 0, 0, "Packet setter does not run message-thread work");
    juce::Thread::sleep(75);
    juce::Timer::callPendingTimersSynchronously();
    near(ack->getValue(), value, 0, "Message-thread timer echoes an exact 24-bit word");
    ack->setValue(0);
    near(ack->getValue(), value, 0, "Host cannot write acknowledgement");
    juce::MemoryBlock state; p.getStateInformation(state);
    PocketBalanceProcessor restored; restored.setStateInformation(state.getData(), int(state.getSize()));
    near(restored.getParameters()[73]->getValue(), 0, 0, "Packets are not saved in project state");
    near(restored.getParameters()[74]->getValue(), 0, 0, "Acknowledgements are not saved in project state");
}
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    testScopeQueue();
    testScopeTimingAndStereo();
    testScopeProcessedSignals();
    testScopeDoesNotChangeAudio();
    testSourceIdentityUiAndRecall();
    testAsyncDropdownRefresh();
    testMixedRoutingAndRates();
    testClashWorkflow();
    testSourceSyncProbe();
    PocketBalanceProcessor p;
    require(p.getBusCount(true) == 16 && p.getTotalNumInputChannels() == 32, "Expose 16 stereo input buses");
    require(p.getTotalNumOutputChannels() == 2, "Expose one stereo output");
    auto layout = p.getBusesLayout(); layout.inputBuses.getReference(3) = juce::AudioChannelSet::mono();
    require(p.isBusesLayoutSupported(layout), "Accept mono auxiliary input");
    layout.inputBuses.getReference(4) = juce::AudioChannelSet::disabled();
    require(p.isBusesLayoutSupported(layout), "Accept disabled auxiliary input");
    layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
    require(!p.isBusesLayoutSupported(layout), "Reject unsupported output layout");
    p.setParameterValue("output", 0); p.setParameterValue("guard", 0);
    p.prepareToPlay(48000, 128);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer(32, 511);
    std::array<float, 16> amplitudes {}; amplitudes[0] = 0.2f; amplitudes[15] = 0.3f;
    fill(buffer, amplitudes, 0, 48000);
    const float expectedL = buffer.getSample(0, 100) + buffer.getSample(30, 100);
    const float expectedR = buffer.getSample(1, 100) + buffer.getSample(31, 100);
    p.processBlock(buffer, midi);
    near(buffer.getSample(0, 100), expectedL, 0.00001f, "Main and input 16 sum without alias corruption");
    near(buffer.getSample(1, 100), expectedR, 0.00001f, "Stereo right preserved");
    p.setParameterValue(p.laneId(15, "mute"), 1);
    for (int i = 0; i < 20; ++i) { fill(buffer, amplitudes, i * 511, 48000); p.processBlock(buffer, midi); }
    fill(buffer, amplitudes, 0, 48000); const float mainOnly = buffer.getSample(0, 100); p.processBlock(buffer, midi);
    near(buffer.getSample(0, 100), mainOnly, 0.00002f, "Mute silences selected lane");
    p.setParameterValue(p.laneId(15, "mute"), 0); p.setParameterValue(p.laneId(15, "solo"), 1);
    for (int i = 0; i < 20; ++i) { fill(buffer, amplitudes, i * 511, 48000); p.processBlock(buffer, midi); }
    fill(buffer, amplitudes, 0, 48000); const float soloOnly = buffer.getSample(30, 100); p.processBlock(buffer, midi);
    near(buffer.getSample(0, 100), soloOnly, 0.00002f, "Solo isolates selected lane");
    fill(buffer, amplitudes, 0, 48000); p.processBlockBypassed(buffer, midi);
    near(buffer.getSample(0, 100), expectedL, 0.00001f, "Host bypass sums all sources at unity");
    p.resetLevels(); p.setParameterValue("guard", 1);
    amplitudes.fill(0.8f); fill(buffer, amplitudes, 0, 48000); p.processBlock(buffer, midi);
    require(buffer.getMagnitude(0, 511) <= 0.891252f, "Correlated mix is protected");
    buffer.clear(); buffer.setSample(2, 0, std::numeric_limits<float>::quiet_NaN()); p.processBlock(buffer, midi);
    require(std::isfinite(buffer.getSample(0, 0)), "Non-finite source cannot poison output");
    juce::AudioBuffer<float> empty(32, 0); p.processBlock(empty, midi);

    p.prepareToPlay(48000, 128); p.startLearning();
    amplitudes.fill(0); amplitudes[0] = 0.4f; amplitudes[1] = 0.7f; amplitudes[2] = 0.2f; amplitudes[4] = 0.3f;
    int cursor = 0;
    for (int block = 0; cursor < 384000; ++block)
    {
        const int count = std::min(384000 - cursor, block % 2 ? 511 : 37);
        buffer.setSize(32, count); fill(buffer, amplitudes, cursor, 48000); p.processBlock(buffer, midi); cursor += count;
    }
    require(p.hasCapture() && !p.isLearning(), "Eight second capture completes with varying block sizes");
    require(p.getSuggestions().activeLanes == 4, "Capture excludes silent lanes");
    require(p.applyBalance(), "Apply measured balance");
    const float learned = p.value(p.laneId(1, "gain")); p.applyBalance();
    near(p.value(p.laneId(1, "gain")), learned, 0.001f, "Repeated Apply is idempotent");
    require(p.canUndoBalance(), "Repeated Apply keeps the previous undo available");
    p.undoBalance();
    for (int lane = 0; lane < 16; ++lane)
        near(p.value(p.laneId(lane, "gain")), 0, 0.001f, "Repeated Apply preserves the original pre-apply faders for undo");
    require(!p.canUndoBalance(), "Undo consumes its stored fader snapshot");
    require(p.applyBalance(), "Apply the captured balance after undo");
    p.setLaneName(1, "Long 808"); p.setParameterValue("duck", 3.5f);
    juce::MemoryBlock state; p.getStateInformation(state);
    PocketBalanceProcessor restored; restored.setStateInformation(state.getData(), int(state.getSize()));
    near(restored.value(p.laneId(1, "gain")), learned, 0.001f, "Recall learned gains");
    near(restored.value("duck"), 3.5f, 0.001f, "Recall duck amount");
    require(restored.laneName(1) == "Long 808", "Recall lane names");
    require(!restored.hasCapture() && !restored.isLearning(), "Do not recall stale analysis or active capture");
    p.setParameterValue(p.laneId(0, "gain"), -9); p.applyBalance(); p.undoBalance();
    near(p.value(p.laneId(0, "gain")), -9, 0.001f, "Undo restores previous fader levels");
    p.resetLevels();
    require(p.canUndoBalance(), "Reset supplies an undo snapshot before state recall");
    p.compare.store(true);
    p.setStateInformation(state.getData(), int(state.getSize()));
    require(!p.canUndoBalance() && !p.compare.load(), "State recall invalidates stale undo and comparison");
    p.undoBalance();
    near(p.value(p.laneId(1, "gain")), learned, 0.001f, "Undo after state recall cannot restore stale faders");
    const juce::String snapshotMode = argc > 1 ? argv[1] : "";
    if (argc == 3 && (snapshotMode == "--snapshot" || snapshotMode == "--snapshot-small"
                      || snapshotMode == "--snapshot-large"))
    {
        p.resetLevels(); p.setParameterValue("duck", 6); p.setParameterValue("output", -6);
        p.setParameterValue("guard", 1);
        p.setLaneName(0, "Kick - Round 808"); p.setLaneName(1, "808 - Warm sustained bass");
        p.setLaneName(2, "Snare - Tight room"); p.setLaneName(4, "Closed hat - Soft ticks");
        p.setParameterValue(p.laneId(0, "role"), 0); p.setParameterValue(p.laneId(1, "role"), 1);
        p.setParameterValue(p.laneId(2, "role"), 2); p.setParameterValue(p.laneId(4, "role"), 4);
        p.prepareToPlay(48000, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        if (snapshotMode == "--snapshot-small") editor->setSize(1120, 880);
        if (snapshotMode == "--snapshot-large") editor->setSize(1600, 1100);
        std::uint32_t noiseState = 0x12345678u;
        constexpr int previewSamples = 172800; // 3.6 seconds, within the preallocated queue.
        for (int start = 0; start < previewSamples; start += 512)
        {
            const int samples = std::min(512, previewSamples - start);
            buffer.setSize(32, samples); buffer.clear();
            for (int n = 0; n < samples; ++n)
            {
                const double t = double(start + n) / 48000.0;
                const float beat = float(std::fmod(t, 0.5));
                const float backbeat = float(std::fmod(t + 0.25, 0.5));
                const float tick = float(std::fmod(t, 0.125));
                noiseState = noiseState * 1664525u + 1013904223u;
                const float noise = float((noiseState >> 8) & 0xffffu) / 32767.5f - 1.0f;
                const float kick = 0.85f * std::exp(-beat * 24.0f)
                    * std::sin(6.28318530718f * (54.0f * beat + 1.2f * (1.0f - std::exp(-beat * 35.0f))));
                const float bass = 0.35f * (float(std::sin(t * 6.28318530718 * 43.6535))
                    + 0.2f * float(std::sin(t * 6.28318530718 * 87.307)));
                const float snare = 0.30f * std::exp(-backbeat * 38.0f) * noise;
                const float hat = 0.13f * std::exp(-tick * 100.0f) * noise;
                buffer.setSample(0, n, kick); buffer.setSample(1, n, kick);
                buffer.setSample(2, n, bass); buffer.setSample(3, n, bass);
                buffer.setSample(4, n, snare); buffer.setSample(5, n, snare * 0.86f);
                buffer.setSample(8, n, hat * 0.75f); buffer.setSample(9, n, hat);
            }
            p.processBlock(buffer, midi);
        }
        auto* view = dynamic_cast<PocketBalanceEditor*>(editor.get());
        require(view != nullptr, "Snapshot uses the native Pocket Balance editor");
        view->refreshDisplay();
        auto screenshot = editor->createComponentSnapshot(editor->getLocalBounds());
        juce::FileOutputStream stream { juce::File(juce::String(argv[2])) };
        require(stream.openedOk(), "Open snapshot destination");
        juce::PNGImageFormat png; require(png.writeImageToStream(screenshot, stream), "Save actual native plugin UI");
    }
    if (argc == 3 && snapshotMode.startsWith("--snapshot-clashes"))
    {
        PocketBalanceProcessor visual;
        visual.setParameterValue("guard", 0); visual.setParameterValue("output", -6);
        visual.setLaneName(0, "Clean 808"); visual.setLaneName(1, "Floor tom");
        visual.setParameterValue(visual.laneId(0, "role"), 1); visual.setParameterValue(visual.laneId(1, "role"), 5);
        visual.prepareToPlay(48000, 511); captureClashLoop(visual);
        visual.parameters.state.setProperty("analysisView", 2, nullptr);
        std::unique_ptr<juce::AudioProcessorEditor> editor(visual.createEditor());
        if (snapshotMode.endsWith("small")) editor->setSize(1120, 880);
        if (snapshotMode.endsWith("large")) editor->setSize(1600, 1100);
        auto* view = dynamic_cast<PocketBalanceEditor*>(editor.get());
        view->refreshDisplay(); visual.previewClash(0, 2); view->refreshDisplay();
        auto screenshot = editor->createComponentSnapshot(editor->getLocalBounds());
        juce::FileOutputStream stream { juce::File(juce::String(argv[2])) };
        require(stream.openedOk(), "Open clash snapshot destination");
        juce::PNGImageFormat png; require(png.writeImageToStream(screenshot, stream), "Save actual clash UI snapshot");
    }
    std::cout << "PASS: scope stereo ranges, timing, duck/mute/compare/bypass/output, SPSC overflow/concurrency, audio invariance; source identity inference, manual override and open-editor state recall; async waveform and role dropdown refresh ordering; 16 buses, mixed mono/disabled routing, disabled solo, variable blocks and 44.1/48/96 kHz capture, mute/solo, bypass, protection, apply, undo, state\n";
}
