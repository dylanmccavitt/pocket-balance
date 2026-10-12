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
    }
    std::array<const float*, pocket::laneCount> left {}, right {};
    std::array<float, pocket::laneCount> target {}, peaks {};
    std::array<int, pocket::laneCount> roles {};
    std::array<bool, pocket::laneCount> audible {};
    bool anySolo = false;
    for (int i = 0; i < pocket::laneCount; ++i)
        anySolo |= getBus(true, i)->isEnabled() && laneParameters[size_t(i)].solo->load() > 0.5f;
    const bool comparing = compare.load();
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
    float outMeter = outputPeak.load(), maxReduction = 0, maxDuck = 0;
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
        float sumL = 0, sumR = 0;
        if (scopeActive)
            for (std::size_t i = pocket::scopeKick; i < pocket::scopeWaveCount; ++i)
                scopeLeft[i] = scopeRight[i] = 0;
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            const auto index = size_t(i);
            const float attenuation = !bypass && roles[index] == 1 ? duckGain : 1;
            const float processedL = l[index] * attenuation, processedR = r[index] * attenuation;
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
                    scopeLeft[pocket::scopeBassBefore] += l[index];
                    scopeRight[pocket::scopeBassBefore] += r[index];
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
            capturing = false; learning.store(false); learnProgress.store(1);
            ready.store(true, std::memory_order_release);
        }
    }
    if (capturing) learnProgress.store(float(captureSamples) / float(captureLength));
    for (int i = 0; i < pocket::laneCount; ++i) meters[size_t(i)].store(peaks[size_t(i)]);
    outputPeak.store(outMeter); reduction.store(maxReduction); duckReduction.store(maxDuck);
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
    canUndo = true; compare.store(false); return true;
}
void PocketBalanceProcessor::undoBalance()
{
    if (!canUndo) return;
    for (int i = 0; i < pocket::laneCount; ++i) setParameterValue(laneId(i, "gain"), undoGains[size_t(i)]);
    canUndo = false;
}
void PocketBalanceProcessor::resetLevels()
{
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        undoGains[size_t(i)] = value(laneId(i, "gain")); setParameterValue(laneId(i, "gain"), 0);
        setParameterValue(laneId(i, "mute"), 0); setParameterValue(laneId(i, "solo"), 0);
    }
    canUndo = true; compare.store(false);
}
juce::String PocketBalanceProcessor::laneName(int i) const
{ return parameters.state.getProperty("name" + juce::String(i), "Drum " + juce::String(i + 1).paddedLeft('0', 2)).toString(); }
void PocketBalanceProcessor::setLaneName(int i, const juce::String& name)
{ parameters.state.setProperty("name" + juce::String(i), name.trim().substring(0, 64), nullptr); }
void PocketBalanceProcessor::getStateInformation(juce::MemoryBlock& data)
{ if (auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, data); }
void PocketBalanceProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(parameters.state.getType()))
        {
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
            ready.store(false); learnCommand.store(2); compare.store(false); canUndo.store(false);
        }
}
juce::AudioProcessorEditor* PocketBalanceProcessor::createEditor() { return new PocketBalanceEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PocketBalanceProcessor(); }
