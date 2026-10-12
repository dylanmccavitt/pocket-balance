// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::String PocketBalanceProcessor::laneId(int lane, const char* field)
{ return "lane" + juce::String(lane + 1) + "_" + field; }

juce::AudioProcessor::BusesProperties PocketBalanceProcessor::makeBuses()
{
    BusesProperties buses;
    for (int i = 0; i < pocket::laneCount; ++i)
        buses.addBus(true, "Drum " + juce::String(i + 1).paddedLeft('0', 2), juce::AudioChannelSet::stereo(), true);
    buses.addBus(false, "Drum mix", juce::AudioChannelSet::stereo(), true);
    return buses;
}

juce::AudioProcessorValueTreeState::ParameterLayout PocketBalanceProcessor::makeParameters()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    juce::StringArray roles;
    for (auto name : pocket::roleNames) roles.add(name);
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        const auto name = "Drum " + juce::String(i + 1) + " ";
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { laneId(i, "gain"), 1 },
            name + "level", juce::NormalisableRange<float>(-24, 12, 0.1f), 0.0f));
        layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { laneId(i, "role"), 1 },
            name + "role", roles, i < 8 ? i : 7));
        layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { laneId(i, "mute"), 1 }, name + "mute", false));
        layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { laneId(i, "solo"), 1 }, name + "solo", false));
    }
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "output", 1 }, "Output level",
        juce::NormalisableRange<float>(-24, 6, 0.1f), -6.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "duck", 1 }, "Kick to 808 ducking",
        juce::NormalisableRange<float>(0, 12, 0.1f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "amount", 1 }, "Balance amount",
        juce::NormalisableRange<float>(0, 100, 1), 70.0f));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { "guard", 1 }, "Peak guard", true));
    // Append only: retain all existing host parameter indices and IDs.
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { "clash_enabled", 1 }, "Frequency correction enabled", false));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { "clash_source", 1 }, "Frequency correction source", 0, pocket::laneCount - 1, 0));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { "clash_trigger", 1 }, "Frequency correction protected source", 0, pocket::laneCount - 1, 1));
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { "clash_band", 1 }, "Frequency correction band", 0, pocket::clashBandCount - 1, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "clash_depth", 1 }, "Frequency correction depth",
        juce::NormalisableRange<float>(0, 3, 0.1f), 2.0f));
    return layout;
}

PocketBalanceProcessor::PocketBalanceProcessor()
    : AudioProcessor(makeBuses()), parameters(*this, nullptr, "PocketBalanceState", makeParameters())
{
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        laneParameters[size_t(i)] = { parameters.getRawParameterValue(laneId(i, "gain")),
            parameters.getRawParameterValue(laneId(i, "role")), parameters.getRawParameterValue(laneId(i, "mute")),
            parameters.getRawParameterValue(laneId(i, "solo")) };
        meters[size_t(i)].store(0); connected[size_t(i)].store(false);
        capturedRms[size_t(i)].store(-120); capturedPeak[size_t(i)].store(-120);
        capturedValid[size_t(i)].store(false);
    }
    outputParameter = parameters.getRawParameterValue("output");
    duckParameter = parameters.getRawParameterValue("duck");
    guardParameter = parameters.getRawParameterValue("guard");
    int ci = 0;
    for (const auto* id : { "clash_enabled", "clash_source", "clash_trigger", "clash_band", "clash_depth" })
        clashParameters[size_t(ci++)] = parameters.getRawParameterValue(id);
    // Preserve all 73 existing parameter indices. Transport/readback values are not serialized.
    addParameter(sourceSyncPacket = new SourceSyncParameter(false));
    addParameter(sourceSyncAck = new SourceSyncParameter(true));
    addParameter(sourceSyncContext = new SourceSyncParameter("source_sync_context", true));
    for (int i = 0; i < pocket::laneCount; ++i)
        addParameter(sourceSyncMapParameters[size_t(i)] = new SourceSyncParameter("source_sync_map" + juce::String(i + 1), true));
    sourceTracks.fill(-1);
    refreshSourceContext();
    startTimerHz(30);
}

PocketBalanceProcessor::~PocketBalanceProcessor() { stopTimer(); }

void PocketBalanceProcessor::timerCallback()
{
    const juce::ScopedLock lock(sourceSyncLock);
    sourceSyncAck->flushHostValue(); sourceSyncContext->flushHostValue();
    for (auto* parameter : sourceSyncMapParameters) parameter->flushHostValue();
    std::uint32_t word = 0;
    if (!sourceSyncPacket->takePacket(word)) return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (sourceCompanionHeard && now - sourceLastContact > 10000.0)
    {
        sourceReceiver.clear();
        if (sourceValidRoutes != 0) invalidateSourceCapture();
        sourceValidRoutes = 0; // Require a fresh STATUS after an interrupted companion session.
    }
    pocket::SourceSyncReceiver::Context context { sourceContextWord, sourceTracks, sourceValidRoutes };
    const auto result = sourceReceiver.accept(word, context);
    if (result == pocket::SourceSyncReceiver::Result::rejected)
    { sourceSyncRejected = true; return; }
    sourceCompanionHeard = true; sourceLastContact = now; sourceSyncRejected = false;
    if (result == pocket::SourceSyncReceiver::Result::routeStatus)
    {
        std::uint16_t mapped = 0;
        for (int i = 0; i < pocket::laneCount; ++i)
            if (sourceTracks[size_t(i)] >= 0) mapped |= static_cast<std::uint16_t>(1u << i);
        const auto valid = static_cast<std::uint16_t>(word & mapped);
        if (valid != sourceValidRoutes) invalidateSourceCapture();
        sourceValidRoutes = valid;
    }
    else if (result == pocket::SourceSyncReceiver::Result::nameReady)
    {
        // Preserve exact Mixer text. Do not invoke the editor's Auto-role inference.
        const auto lane = sourceReceiver.completedLane();
        const auto autoKey = "autoRole" + juce::String(lane);
        if (!parameters.state.hasProperty(autoKey))
            parameters.state.setProperty(autoKey, laneName(lane).startsWith("Drum "), nullptr);
        parameters.state.setProperty("name" + juce::String(lane),
            juce::String::fromUTF8(sourceReceiver.completedName(), sourceReceiver.completedNameBytes()), nullptr);
    }
    sourceSyncAck->publishAcknowledgement(word);
}

std::array<int, pocket::laneCount> PocketBalanceProcessor::sourceMapping() const
{ const juce::ScopedLock lock(sourceSyncLock); return sourceTracks; }
int PocketBalanceProcessor::sourceOwnerTrack() const
{ const juce::ScopedLock lock(sourceSyncLock); return sourceDestinationTrack; }
int PocketBalanceProcessor::sourceOwnerSlot() const
{ const juce::ScopedLock lock(sourceSyncLock); return sourceDestinationSlot; }
std::uint32_t PocketBalanceProcessor::getSourceSyncContextWord() const
{ const juce::ScopedLock lock(sourceSyncLock); return sourceContextWord; }

void PocketBalanceProcessor::invalidateSourceCapture()
{
    ready.store(false); learnCommand.store(2);
    stopClashPreview(); clashCaptureStable.store(false);
}

void PocketBalanceProcessor::refreshSourceContext()
{
    sourceRevision = (sourceRevision + 1u) & 255u;
    const bool linked = sourceDestinationTrack >= 0 && sourceDestinationSlot >= 0;
    sourceContextWord = (sourceRevision << 15) | (linked ? 1u << 14 : 0u)
        | static_cast<unsigned>(sourceDestinationTrack + 1)
        | (static_cast<unsigned>(sourceDestinationSlot + 1) << 10);
    sourceReceiver.clear(); sourceSyncPacket->discardPacket();
    sourceSyncAck->publishAcknowledgement(0);
    sourceValidRoutes = 0; sourceCompanionHeard = false; sourceSyncRejected = false;
    // Publish mappings before context. The companion reads context twice around the map.
    for (int i = 0; i < pocket::laneCount; ++i)
        sourceSyncMapParameters[size_t(i)]->publishAcknowledgement(static_cast<std::uint32_t>(sourceTracks[size_t(i)] + 1) << 14);
    sourceSyncContext->publishAcknowledgement(sourceContextWord);
}

void PocketBalanceProcessor::setSourceMapping(int ownerTrack, int ownerSlot,
                                             const std::array<int, pocket::laneCount>& mapping)
{
    const juce::ScopedLock lock(sourceSyncLock);
    if (ownerTrack < 0 || ownerTrack > 500 || ownerSlot < 0 || ownerSlot > 9)
    { clearSourceMapping(); return; }
    sourceDestinationTrack = ownerTrack; sourceDestinationSlot = ownerSlot;
    bool any = false;
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        const int track = mapping[size_t(i)];
        sourceTracks[size_t(i)] = track >= 0 && track <= 500 ? track : -1;
        any |= sourceTracks[size_t(i)] >= 0;
    }
    if (!any) { clearSourceMapping(); return; }
    auto previous = parameters.state.getChildWithName("SourceSync");
    if (previous.isValid()) parameters.state.removeChild(previous, nullptr);
    juce::ValueTree mappingState("SourceSync");
    mappingState.setProperty("schema", 1, nullptr);
    mappingState.setProperty("ownerTrack", ownerTrack, nullptr);
    mappingState.setProperty("ownerSlot", ownerSlot, nullptr);
    for (int i = 0; i < pocket::laneCount; ++i)
        mappingState.setProperty("lane" + juce::String(i), sourceTracks[size_t(i)], nullptr);
    parameters.state.appendChild(mappingState, nullptr);
    refreshSourceContext(); invalidateSourceCapture();
}

void PocketBalanceProcessor::clearSourceMapping()
{
    const juce::ScopedLock lock(sourceSyncLock);
    auto mappingState = parameters.state.getChildWithName("SourceSync");
    if (mappingState.isValid()) parameters.state.removeChild(mappingState, nullptr);
    sourceTracks.fill(-1); sourceDestinationTrack = -1; sourceDestinationSlot = -1;
    refreshSourceContext(); invalidateSourceCapture();
}

void PocketBalanceProcessor::restoreSourceMapping()
{
    sourceTracks.fill(-1); sourceDestinationTrack = -1; sourceDestinationSlot = -1;
    const auto mappingState = parameters.state.getChildWithName("SourceSync");
    if (mappingState.isValid())
    {
        const auto integer = [&mappingState](const juce::Identifier& key, int low, int high, int& result)
        {
            const auto text = mappingState.getProperty(key).toString();
            const auto value = text.getIntValue();
            if (text != juce::String(value) || value < low || value > high) return false;
            result = value; return true;
        };
        int schema = 0, owner = -1, slot = -1;
        auto mapping = sourceTracks;
        bool valid = integer("schema", 1, 1, schema) && integer("ownerTrack", 0, 500, owner)
            && integer("ownerSlot", 0, 9, slot);
        bool any = false;
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            valid &= integer("lane" + juce::String(i), -1, 500, mapping[size_t(i)]);
            any |= mapping[size_t(i)] >= 0;
        }
        if (valid && any) { sourceTracks = mapping; sourceDestinationTrack = owner; sourceDestinationSlot = slot; }
        else parameters.state.removeChild(mappingState, nullptr);
    }
    refreshSourceContext();
}

juce::String PocketBalanceProcessor::sourceSyncStatus() const
{
    const juce::ScopedLock lock(sourceSyncLock);
    if (sourceDestinationTrack < 0) return "FL source names: not linked";
    if (!sourceCompanionHeard) return "FL source names: waiting for companion";
    if (juce::Time::getMillisecondCounterHiRes() - sourceLastContact > 10000.0) return "FL source names: companion offline";
    if (sourceSyncRejected) return "FL source names: retrying transfer";
    int mapped = 0, active = 0;
    for (int i = 0; i < pocket::laneCount; ++i)
        if (sourceTracks[size_t(i)] >= 0) { ++mapped; if ((sourceValidRoutes & (1u << i)) != 0) ++active; }
    if (active < mapped) return "FL source names: " + juce::String(active) + "/" + juce::String(mapped) + " routes connected";
    return "FL source names linked: " + juce::String(mapped) + " inputs";
}

bool PocketBalanceProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.outputBuses.size() != 1 || layouts.inputBuses.size() != pocket::laneCount
        || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    for (int i = 0; i < layouts.inputBuses.size(); ++i)
    {
        const auto& set = layouts.inputBuses[i];
        if (set != juce::AudioChannelSet::mono() && set != juce::AudioChannelSet::stereo()
            && !set.isDisabled()) return false;
    }
    return !layouts.getMainInputChannelSet().isDisabled();
}

void PocketBalanceProcessor::prepareToPlay(double rate, int)
{
    rate = rate > 0 ? rate : 48000.0;
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        analysers[size_t(i)].prepare(rate);
        gainSmoothers[size_t(i)].prepare(rate);
        gainSmoothers[size_t(i)].reset(pocket::gainFromDb(laneParameters[size_t(i)].gain->load()));
        meters[size_t(i)].store(0);
    }
    outputSmoother.prepare(rate); outputSmoother.reset(pocket::gainFromDb(outputParameter->load()));
    duckSmoother.prepare(rate); duckSmoother.reset(duckParameter->load());
    duck.prepare(rate); guard.prepare(rate);
    clashAnalyser.prepare(rate); clashCorrection.prepare(rate);
    stopClashPreview(); clashCaptureStable.store(false); clashReduction.store(0);
    scope.prepare(rate);
    meterDecay = std::exp(-1.0f / float(rate * 0.35));
    captureLength = std::int64_t(rate * 8.0);
    captureSamples = 0; learning.store(false); ready.store(false); learnProgress.store(0);
    outputPeak.store(0); reduction.store(0); duckReduction.store(0);
    setLatencySamples(0);
}

void PocketBalanceProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{ midi.clear(); process(buffer, false); }
void PocketBalanceProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{ midi.clear(); process(buffer, true); }

void PocketBalanceProcessor::process(juce::AudioBuffer<float>& buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    const int samples = buffer.getNumSamples();
    if (samples == 0 || buffer.getNumChannels() < 2) return;
    const bool scopeActive = scope.beginBlock();
    if (!scopeActive) scope.skipSamples(samples);
    const int command = learnCommand.exchange(0);
    if (command != 0)
    {
        ready.store(false); learning.store(command == 1); captureSamples = 0; learnProgress.store(0);
        for (auto& analyser : analysers) analyser.reset();
        clashAnalyser.reset();
        learningClashSettings = currentClashSettings();
        learningClashStable = !compare.load() && !bypass;
        clashCaptureStable.store(false);
    }
    std::array<const float*, pocket::laneCount> left {}, right {};
    std::array<float, pocket::laneCount> target {}, peaks {};
    std::array<int, pocket::laneCount> roles {};
    std::array<bool, pocket::laneCount> audible {};
    bool anySolo = false;
    for (int i = 0; i < pocket::laneCount; ++i)
        anySolo |= getBus(true, i)->isEnabled() && laneParameters[size_t(i)].solo->load() > 0.5f;
    const bool comparing = compare.load();
    if (comparing || bypass || (isClashPreviewing() && !hasFreshClashCapture())) stopClashPreview();
    const auto preview = previewConfig.load();
    pocket::ClashConfig coherent;
    if (readAppliedClash(coherent)) audioAppliedClash = coherent;
    clashCorrection.configure(preview != 0 ? unpackClash(preview) : audioAppliedClash);
    if (learning.load() && (currentClashSettings() != learningClashSettings || comparing || bypass))
        learningClashStable = false;
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        const auto index = size_t(i);
        auto input = getBusBuffer(buffer, true, i);
        const int channels = input.getNumChannels();
        connected[index].store(channels > 0);
        if (channels > 0) { left[index] = input.getReadPointer(0); right[index] = input.getReadPointer(channels > 1 ? 1 : 0); }
        const auto& p = laneParameters[index];
        audible[index] = bypass || (p.mute->load() < 0.5f && (!anySolo || p.solo->load() > 0.5f));
        target[index] = audible[index] ? pocket::gainFromDb(comparing || bypass ? 0 : p.gain->load()) : 0;
        roles[index] = int(p.role->load());
        peaks[index] = meters[index].load();
    }
    auto output = getBusBuffer(buffer, false, 0);
    auto* outLeft = output.getWritePointer(0);
    auto* outRight = output.getWritePointer(1);
    const float outputTarget = pocket::gainFromDb(outputParameter->load());
    const float duckTarget = comparing || bypass ? 0 : duckParameter->load();
    const bool guardOn = !bypass && guardParameter->load() > 0.5f;
    float outMeter = outputPeak.load(), maxReduction = 0, maxDuck = 0, maxClash = 0;
    bool capturing = learning.load();
    std::array<float, pocket::scopeWaveCount> scopeLeft {}, scopeRight {};
    for (int sample = 0; sample < samples; ++sample)
    {
        std::array<float, pocket::laneCount> l {}, r {};
        float kick = 0;
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            const auto index = size_t(i);
            const float rawL = left[index] ? pocket::clean(left[index][sample]) : 0;
            const float rawR = right[index] ? pocket::clean(right[index][sample]) : 0;
            if (capturing) analysers[index].push(rawL, rawR);
            peaks[index] = std::max(std::max(std::abs(rawL), std::abs(rawR)), peaks[index] * meterDecay);
            const float gain = bypass ? 1.0f : gainSmoothers[index].next(target[index]);
            l[index] = rawL * gain; r[index] = rawR * gain;
            if (roles[index] == 0 && audible[index]) kick = std::max(kick, std::max(std::abs(l[index]), std::abs(r[index])));
        }
        const float duckGain = duck.next(kick, duckSmoother.next(duckTarget));
        maxDuck = std::max(maxDuck, -pocket::dbFromGain(duckGain));
        // Gather and duck every lane before correction: input and output buses can alias.
        const auto beforeDuckL = l, beforeDuckR = r;
        for (int i = 0; i < pocket::laneCount; ++i)
            if (!bypass && roles[size_t(i)] == 1)
            { l[size_t(i)] *= duckGain; r[size_t(i)] *= duckGain; }
        if (capturing) clashAnalyser.push(l, r);
        clashCorrection.process(l, r, bypass || comparing);
        maxClash = std::max(maxClash, clashCorrection.reductionDb());
        float sumL = 0, sumR = 0;
        if (scopeActive)
            for (std::size_t i = pocket::scopeKick; i < pocket::scopeWaveCount; ++i)
                scopeLeft[i] = scopeRight[i] = 0;
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            const auto index = size_t(i);
            const float processedL = l[index], processedR = r[index];
            sumL += processedL; sumR += processedR;
            if (scopeActive)
            {
                scopeLeft[index] = processedL; scopeRight[index] = processedR;
                if (roles[index] == 0)
                {
                    scopeLeft[pocket::scopeKick] += processedL;
                    scopeRight[pocket::scopeKick] += processedR;
                }
                else if (roles[index] == 1)
                {
                    scopeLeft[pocket::scopeBassBefore] += beforeDuckL[index];
                    scopeRight[pocket::scopeBassBefore] += beforeDuckR[index];
                    scopeLeft[pocket::scopeBassAfter] += processedL;
                    scopeRight[pocket::scopeBassAfter] += processedR;
                }
            }
        }
        const float outGain = bypass ? 1.0f : outputSmoother.next(outputTarget);
        sumL *= outGain; sumR *= outGain;
        maxReduction = std::max(maxReduction, guard.process(sumL, sumR, guardOn));
        outLeft[sample] = sumL; outRight[sample] = sumR;
        if (scopeActive)
        {
            scopeLeft[pocket::scopeOutput] = sumL; scopeRight[pocket::scopeOutput] = sumR;
            scope.pushSample(scopeLeft, scopeRight, bypass ? 0.0f : -pocket::dbFromGain(duckGain));
        }
        outMeter = std::max(std::max(std::abs(sumL), std::abs(sumR)), outMeter * meterDecay);
        if (capturing && ++captureSamples >= captureLength)
        {
            for (int i = 0; i < pocket::laneCount; ++i)
            {
                const auto m = analysers[size_t(i)].finish();
                capturedRms[size_t(i)].store(m.rmsDb); capturedPeak[size_t(i)].store(m.peakDb);
                capturedValid[size_t(i)].store(m.valid);
            }
            publishClashes(clashAnalyser.finish());
            for (size_t i = 0; i < learningClashSettings.size(); ++i)
                capturedClashSettings[i].store(learningClashSettings[i]);
            clashCaptureStable.store(learningClashStable);
            capturing = false; learning.store(false); learnProgress.store(1);
            ready.store(true, std::memory_order_release);
        }
    }
    if (capturing) learnProgress.store(float(captureSamples) / float(captureLength));
    for (int i = 0; i < pocket::laneCount; ++i) meters[size_t(i)].store(peaks[size_t(i)]);
    outputPeak.store(outMeter); reduction.store(maxReduction); duckReduction.store(maxDuck); clashReduction.store(maxClash);
}

pocket::Suggestions PocketBalanceProcessor::getSuggestions() const
{
    if (!hasCapture()) return {};
    std::array<pocket::Measurement, pocket::laneCount> measurements;
    std::array<int, pocket::laneCount> roles;
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        const auto index = size_t(i);
        measurements[index] = { capturedRms[index].load(), capturedPeak[index].load(), capturedValid[index].load() };
        roles[index] = int(laneParameters[index].role->load());
    }
    return pocket::suggest(measurements, roles);
}

void PocketBalanceProcessor::setParameterValue(const juce::String& id, float actual)
{
    if (auto* p = parameters.getParameter(id))
    {
        p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(actual)); p->endChangeGesture();
    }
}
bool PocketBalanceProcessor::applyBalance()
{
    const auto suggestions = getSuggestions();
    if (suggestions.activeLanes < 2) return false;
    const float amount = value("amount") * 0.01f;
    bool changed = false;
    for (int i = 0; i < pocket::laneCount; ++i)
        changed |= suggestions.valid[size_t(i)] && std::abs(value(laneId(i, "gain"))
            - suggestions.gainDb[size_t(i)] * amount) > 0.051f;
    if (!changed) return true;
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        undoGains[size_t(i)] = value(laneId(i, "gain"));
        if (suggestions.valid[size_t(i)]) setParameterValue(laneId(i, "gain"), suggestions.gainDb[size_t(i)] * amount);
    }
    canUndo = true; compare.store(false); stopClashPreview(); clashCaptureStable.store(false); return true;
}
void PocketBalanceProcessor::undoBalance()
{
    if (!canUndo) return;
    for (int i = 0; i < pocket::laneCount; ++i) setParameterValue(laneId(i, "gain"), undoGains[size_t(i)]);
    canUndo = false; stopClashPreview(); clashCaptureStable.store(false);
}
void PocketBalanceProcessor::resetLevels()
{
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        undoGains[size_t(i)] = value(laneId(i, "gain")); setParameterValue(laneId(i, "gain"), 0);
        setParameterValue(laneId(i, "mute"), 0); setParameterValue(laneId(i, "solo"), 0);
    }
    canUndo = true; compare.store(false); stopClashPreview(); clashCaptureStable.store(false);
}
juce::String PocketBalanceProcessor::laneName(int i) const
{ return parameters.state.getProperty("name" + juce::String(i), "Drum " + juce::String(i + 1).paddedLeft('0', 2)).toString(); }
void PocketBalanceProcessor::setLaneName(int i, const juce::String& name)
{ parameters.state.setProperty("name" + juce::String(i), name.trim().substring(0, 64), nullptr); }
void PocketBalanceProcessor::getStateInformation(juce::MemoryBlock& data)
{ const juce::ScopedLock lock(sourceSyncLock); if (auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, data); }
void PocketBalanceProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(parameters.state.getType()))
        {
            const juce::ScopedLock lock(sourceSyncLock);
            auto state = juce::ValueTree::fromXml(*xml);
            // Older sessions must clear a correction already enabled in this instance.
            const std::array<const char*, 5> ids { "clash_enabled", "clash_source", "clash_trigger", "clash_band", "clash_depth" };
            const std::array<float, 5> defaults { 0, 0, 1, 0, 2 };
            for (size_t i = 0; i < ids.size(); ++i)
                if (!state.getChildWithProperty("id", ids[i]).isValid())
                {
                    juce::ValueTree param("PARAM");
                    param.setProperty("id", ids[i], nullptr); param.setProperty("value", defaults[i], nullptr);
                    state.appendChild(param, nullptr);
                }
            clashWriters.fetch_add(1);
            parameters.replaceState(state);
            restoreSourceMapping();
            clashGeneration.fetch_add(1); clashWriters.fetch_sub(1);
            ready.store(false); learnCommand.store(2); compare.store(false); canUndo.store(false);
            stopClashPreview(); clashUndoReady.store(false); clashCaptureStable.store(false);
        }
}
void PocketBalanceProcessor::startLearning()
{
    if (compare.load()) return;
    stopClashPreview(); ready.store(false); clashCaptureStable.store(false); learnCommand.store(1);
}
void PocketBalanceProcessor::cancelLearning()
{ stopClashPreview(); ready.store(false); clashCaptureStable.store(false); learnCommand.store(2); }

std::array<float, PocketBalanceProcessor::captureSettingCount> PocketBalanceProcessor::currentClashSettings() const
{
    std::array<float, captureSettingCount> settings {};
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        const auto& p = laneParameters[size_t(i)]; const auto offset = size_t(i * 5);
        settings[offset] = p.gain->load(); settings[offset + 1] = p.role->load();
        settings[offset + 2] = p.mute->load(); settings[offset + 3] = p.solo->load();
        settings[offset + 4] = getBus(true, i)->isEnabled() ? 1.0f : 0.0f;
    }
    settings.back() = duckParameter->load(); return settings;
}
bool PocketBalanceProcessor::hasFreshClashCapture() const
{
    if (!hasCapture() || isLearning() || !clashCaptureStable.load()) return false;
    const auto current = currentClashSettings();
    for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != capturedClashSettings[i].load()) return false;
    return true;
}
void PocketBalanceProcessor::publishClashes(const pocket::ClashReport& report)
{
    reportGeneration.fetch_add(1);
    for (int i = 0; i < report.count; ++i)
    {
        const auto& f = report.findings[size_t(i)]; auto& a = clashFindings[size_t(i)];
        a.first.store(f.first); a.second.store(f.second); a.band.store(f.band);
        a.reduceLane.store(f.reduceLane); a.protectLane.store(f.protectLane);
        a.overlapSeconds.store(f.overlapSeconds); a.levelDifferenceDb.store(f.levelDifferenceDb);
        a.strength.store(f.strength); a.episodes.store(f.episodes); a.sustained.store(f.sustained);
    }
    clashFindingCount.store(report.count);
    reportGeneration.fetch_add(1);
}
pocket::ClashReport PocketBalanceProcessor::getClashReport() const
{
    pocket::ClashReport report;
    if (!hasCapture()) return report;
    const auto generation = reportGeneration.load();
    if ((generation & 1u) != 0) return report;
    report.count = std::clamp(clashFindingCount.load(), 0, pocket::maxClashFindings);
    for (int i = 0; i < report.count; ++i)
    {
        const auto& a = clashFindings[size_t(i)]; auto& f = report.findings[size_t(i)];
        f.first = a.first.load(); f.second = a.second.load(); f.band = a.band.load();
        f.reduceLane = a.reduceLane.load(); f.protectLane = a.protectLane.load();
        f.overlapSeconds = a.overlapSeconds.load(); f.levelDifferenceDb = a.levelDifferenceDb.load();
        f.strength = a.strength.load(); f.episodes = a.episodes.load(); f.sustained = a.sustained.load();
    }
    if (generation != reportGeneration.load()) return {};
    return report;
}
std::uint32_t PocketBalanceProcessor::packClash(const pocket::ClashConfig& config)
{
    if (!config.enabled || config.source < 0 || config.source >= pocket::laneCount
        || config.trigger < 0 || config.trigger >= pocket::laneCount || config.source == config.trigger
        || config.band < 0 || config.band >= pocket::clashBandCount || !std::isfinite(config.depthDb)) return 0;
    return 1u | (std::uint32_t(config.source) << 1) | (std::uint32_t(config.trigger) << 5)
        | (std::uint32_t(config.band) << 9) | (std::uint32_t(std::lround(std::clamp(config.depthDb, 0.0f, 3.0f) * 10)) << 12);
}
pocket::ClashConfig PocketBalanceProcessor::unpackClash(std::uint32_t packed)
{
    return { (packed & 1u) != 0, int((packed >> 1) & 15u), int((packed >> 5) & 15u),
        int((packed >> 9) & 7u), float((packed >> 12) & 31u) * 0.1f };
}
pocket::ClashConfig PocketBalanceProcessor::appliedClash() const
{
    pocket::ClashConfig c;
    if (!readAppliedClash(c)) return {};
    return c;
}
bool PocketBalanceProcessor::readAppliedClash(pocket::ClashConfig& result) const
{
    const auto generation = clashGeneration.load();
    if (clashWriters.load() != 0) return false;
    pocket::ClashConfig c { clashParameters[0]->load() > 0.5f, int(clashParameters[1]->load()),
        int(clashParameters[2]->load()), int(clashParameters[3]->load()), clashParameters[4]->load() };
    if (!std::isfinite(c.depthDb)) c.depthDb = 0;
    c.depthDb = std::clamp(c.depthDb, 0.0f, 3.0f);
    if (c.source < 0 || c.source >= pocket::laneCount || c.trigger < 0 || c.trigger >= pocket::laneCount
        || c.source == c.trigger || c.band < 0 || c.band >= pocket::clashBandCount) c.enabled = false;
    if (clashWriters.load() != 0 || generation != clashGeneration.load()) return false;
    result = c; return true;
}
bool PocketBalanceProcessor::previewClash(int findingIndex, float depthDb)
{
    if (!hasFreshClashCapture() || compare.load() || !std::isfinite(depthDb)) return false;
    const auto report = getClashReport();
    if (findingIndex < 0 || findingIndex >= report.count) return false;
    const auto& f = report.findings[size_t(findingIndex)];
    if (f.reduceLane < 0 || f.protectLane < 0) return false;
    previewConfig.store(packClash({ true, f.reduceLane, f.protectLane, f.band, depthDb }));
    return isClashPreviewing();
}
void PocketBalanceProcessor::setClashParameters(const pocket::ClashConfig& c)
{
    // A bounded generation check keeps the audio thread on the last coherent pair.
    // The writer count also handles overlapping host recall and message-thread edits.
    clashWriters.fetch_add(1);
    setParameterValue("clash_enabled", 0);
    setParameterValue("clash_source", float(c.source)); setParameterValue("clash_trigger", float(c.trigger));
    setParameterValue("clash_band", float(c.band)); setParameterValue("clash_depth", c.depthDb);
    setParameterValue("clash_enabled", c.enabled ? 1.0f : 0.0f);
    clashGeneration.fetch_add(1); clashWriters.fetch_sub(1);
}
bool PocketBalanceProcessor::applyClash()
{
    const auto packed = previewConfig.load();
    if (packed == 0 || !hasFreshClashCapture() || compare.load()) return false;
    const auto config = unpackClash(packed), previous = appliedClash();
    if (packClash(previous) != packed)
    {
        undoClashConfig = previous; clashUndoReady.store(true); setClashParameters(config);
    }
    stopClashPreview(); return true;
}
void PocketBalanceProcessor::undoClash()
{
    stopClashPreview();
    if (!clashUndoReady.exchange(false)) return;
    setClashParameters(undoClashConfig);
}
void PocketBalanceProcessor::disableClash()
{
    stopClashPreview(); auto config = appliedClash();
    if (!config.enabled) return;
    undoClashConfig = config; clashUndoReady.store(true);
    config.enabled = false; setClashParameters(config);
}

juce::AudioProcessorEditor* PocketBalanceProcessor::createEditor() { return new PocketBalanceEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PocketBalanceProcessor(); }
