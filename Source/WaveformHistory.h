// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pocket
{
constexpr std::size_t scopeWaveCount = 20;
constexpr std::size_t scopeOutput = 16, scopeKick = 17, scopeBassBefore = 18, scopeBassAfter = 19;
constexpr double scopeFramesPerSecond = 500.0;

struct ScopeRange
{
    float min = 0, max = 0;
};

struct ScopeFrame
{
    // Signed extrema over both channels, preserving antiphase stereo signals.
    // 0..15: post-fader/mute/duck lanes; 16: post-trim/guard output;
    // 17: summed Kick lanes; 18/19: summed Bass lanes before/after ducking.
    std::array<ScopeRange, scopeWaveCount> waves {};
    float duckDb = 0;
    // End of this frame in processed-audio seconds. Gaps indicate dropped or
    // inactive capture intervals; the clock continues across prepare calls.
    double seconds = 0;
};

// One audio-thread producer and one message-thread consumer. The storage is
// allocated once during processor construction, never in an audio callback.
// Queue overflow drops the NEW frame, never a frame the UI might be reading.
class WaveformHistory
{
public:
    static constexpr std::uint32_t capacity = 4096;

    WaveformHistory() : frames(std::make_unique<ScopeFrame[]>(capacity + 1)) {}

    // Message thread only, on the same thread that calls pop(). Enabling a new
    // view drops queued old history by advancing only the consumer cursor.
    void setActive(bool enabled) noexcept
    {
        if (enabled && !active.load(std::memory_order_relaxed))
            read.store(write.load(std::memory_order_acquire), std::memory_order_release);
        active.store(enabled, std::memory_order_release);
    }

    bool pop(ScopeFrame& frame) noexcept
    {
        const auto current = read.load(std::memory_order_relaxed);
        if (current == write.load(std::memory_order_acquire)) return false;
        frame = frames[current];
        read.store(next(current), std::memory_order_release);
        return true;
    }

    // Producer only: prepareToPlay is serialized with processBlock by the
    // host. No queue cursor is reset, so a simultaneous UI pop remains safe.
    void prepare(double rate) noexcept
    {
        sampleRate = std::isfinite(rate) && rate > 0 ? rate : 48000.0;
        samplePeriod = 1.0 / sampleRate;
        framesPerSecond = std::min(scopeFramesPerSecond, sampleRate);
        clearPartial();
    }

    bool beginBlock() noexcept
    {
        if (active.load(std::memory_order_acquire)) return true;
        clearPartial();
        return false;
    }

    void skipSamples(int samples) noexcept
    { elapsed += double(samples) * samplePeriod; }

    void pushSample(const std::array<float, scopeWaveCount>& left,
                    const std::array<float, scopeWaveCount>& right,
                    float duckDb) noexcept
    {
        elapsed += samplePeriod;
        for (std::size_t i = 0; i < scopeWaveCount; ++i)
        {
            const float lo = std::min(left[i], right[i]);
            const float hi = std::max(left[i], right[i]);
            auto& range = partial.waves[i];
            if (!hasSamples) range = { lo, hi };
            else { range.min = std::min(range.min, lo); range.max = std::max(range.max, hi); }
        }
        partial.duckDb = std::max(partial.duckDb, duckDb);
        hasSamples = true;
        phase += framesPerSecond;
        if (phase < sampleRate) return;
        phase -= sampleRate;
        partial.seconds = elapsed;
        const auto current = write.load(std::memory_order_relaxed);
        const auto following = next(current);
        if (following != read.load(std::memory_order_acquire))
        {
            frames[current] = partial;
            write.store(following, std::memory_order_release);
        }
        // Continue the clock/decimation even when full so a dropped interval
        // remains visible in the next successfully delivered timestamp.
        partial.duckDb = 0;
        hasSamples = false;
    }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "Waveform cursors must be lock-free on the target");
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "Waveform activation must be lock-free on the target");
    static std::uint32_t next(std::uint32_t value) noexcept
    { return value == capacity ? 0 : value + 1; }
    void clearPartial() noexcept
    { partial = {}; phase = 0; hasSamples = false; }

    std::unique_ptr<ScopeFrame[]> frames;
    alignas(64) std::atomic<std::uint32_t> write { 0 };
    alignas(64) std::atomic<std::uint32_t> read { 0 };
    std::atomic<bool> active { false };
    ScopeFrame partial;
    double sampleRate = 48000.0, samplePeriod = 1.0 / 48000.0;
    double framesPerSecond = scopeFramesPerSecond, phase = 0, elapsed = 0;
    bool hasSamples = false;
};
}
