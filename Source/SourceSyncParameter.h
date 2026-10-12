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
        : SourceSyncParameter(acknowledgement ? "source_sync_ack" : "source_sync_packet", acknowledgement) {}

    SourceSyncParameter(const juce::String& id, bool meter)
        : AudioProcessorParameterWithID(juce::ParameterID { id, 2 }, id,
              // FL's scripting API omits nonautomatable parameters and meter categories.
              // Metadata output writes are ignored below, despite the host-visible flags.
              juce::AudioProcessorParameterWithIDAttributes().withAutomatable(true)
                  .withCategory(genericParameter)), readOnly(meter) {}

    float getValue() const override { return float(displayed.load()) / 16777216.0f; }
    float getDefaultValue() const override { return 0.0f; }
    float getValueForText(const juce::String&) const override { return 0.0f; }
    juce::String getText(float, int) const override { return "FL source-name link"; }

    void setValue(float value) override
    {
        if (readOnly)
        {
            // A host may cache its attempted write even though we reject it.
            // Restore that cache from the message thread, including while idle.
            if (!std::isfinite(value) || std::abs(value - getValue()) > 0.0f) republishRequested.store(true);
            return;
        }
        if (!std::isfinite(value) || value < 0.0f || value >= 1.0f) return;
        const auto scaled = value * 16777216.0f;
        if (std::abs(scaled - std::floor(scaled)) > 0.0f) return;
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
        republishRequested.store(false);
        sendValueChangedMessageToListeners(getValue());
    }

    void flushHostValue()
    {
        if (republishRequested.exchange(false)) sendValueChangedMessageToListeners(getValue());
    }

    void discardPacket() noexcept { pendingWord.store(noPacket); }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "Source sync needs lock-free 32-bit atomics");
    const bool readOnly;
    std::atomic<std::uint32_t> displayed { 0 };
    std::atomic<bool> republishRequested { false };
    static constexpr std::uint32_t noPacket = 0x1000000;
    std::atomic<std::uint32_t> pendingWord { noPacket };
};
