// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

// A 24-bit word fits exactly in the normalized float used by the host.
// Transport parameters are runtime-only and do not belong to the saved APVTS.
class SourceSyncParameter final : public juce::AudioProcessorParameterWithID
{
public:
    explicit SourceSyncParameter(bool acknowledgement)
        : AudioProcessorParameterWithID(
              juce::ParameterID { acknowledgement ? "source_sync_ack" : "source_sync_packet", 2 },
              acknowledgement ? "source_sync_ack" : "source_sync_packet",
              juce::AudioProcessorParameterWithIDAttributes().withAutomatable(false)
                  .withCategory(acknowledgement ? otherMeter : genericParameter)),
          readOnly(acknowledgement) {}

    float getValue() const override { return float(displayed.load()) / 16777216.0f; }
    float getDefaultValue() const override { return 0.0f; }
    float getValueForText(const juce::String&) const override { return 0.0f; }
    juce::String getText(float, int) const override { return "FL source-name link"; }

    void setValue(float value) override
    {
        if (readOnly || !std::isfinite(value) || value < 0.0f || value >= 1.0f) return;
        const auto scaled = value * 16777216.0f;
        if (scaled != std::floor(scaled)) return;
        // Hosts may call this on the audio thread. Never wait, allocate, or notify here.
        const auto word = static_cast<std::uint32_t>(scaled);
        displayed.store(word);
        pendingWord.store(word);
    }

    bool takePacket(std::uint32_t& word) noexcept
    {
        word = pendingWord.exchange(noPacket);
        return word != noPacket;
    }

    void publishAcknowledgement(std::uint32_t word)
    {
        jassert(readOnly);
        displayed.store(word);
        sendValueChangedMessageToListeners(getValue());
    }

private:
    const bool readOnly;
    std::atomic<std::uint32_t> displayed { 0 };
    static constexpr std::uint32_t noPacket = 0x1000000;
    std::atomic<std::uint32_t> pendingWord { noPacket };
};
