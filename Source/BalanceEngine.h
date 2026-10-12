#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pocket
{
constexpr int laneCount = 16;
constexpr int roleCount = 8;
inline constexpr std::array<const char*, roleCount> roleNames {
    "Kick", "808 / Bass", "Snare", "Clap", "Hi-hat", "Percussion", "Cymbal", "Other" };
// Starting points for active RMS, relative to the kick. These are aesthetic choices.
inline constexpr std::array<float, roleCount> roleOffsets { 0, -2, -1, -3, -10, -6, -9, -4 };
inline float gainFromDb(float db) { return std::pow(10.0f, db / 20.0f); }
inline float dbFromGain(float gain) { return 20.0f * std::log10(std::max(gain, 0.000001f)); }
inline float clean(float x) { return std::isfinite(x) ? std::clamp(x, -64.0f, 64.0f) : 0.0f; }

struct Measurement
{
    float rmsDb = -120.0f, peakDb = -120.0f;
    bool valid = false;
};

class Analyser
{
public:
    void prepare(double rate) { sampleRate = rate; windowSize = std::max(1, int(rate * 0.02)); reset(); }
    void reset()
    {
        energies.fill(0.0); windows.fill(0); energy = 0; samples = 0;
        peak = 0; maxBin = 0;
    }
    void push(float left, float right)
    {
        left = clean(left); right = clean(right);
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        energy += 0.5 * (double(left) * left + double(right) * right);
        if (++samples == windowSize) flush();
    }
    Measurement finish()
    {
        if (samples > 0) flush();
        // Remove silence and tails more than 18 dB below the loudest window.
        const int lowBin = std::max(70, maxBin - 18); // absolute floor: -50 dBFS
        double total = 0; std::uint64_t count = 0;
        for (int b = lowBin; b < int(energies.size()); ++b)
        {
            total += energies[size_t(b)]; count += windows[size_t(b)];
        }
        Measurement m;
        m.peakDb = dbFromGain(peak);
        m.valid = count >= std::uint64_t(sampleRate * 0.05) && total > 0;
        if (m.valid) m.rmsDb = dbFromGain(float(std::sqrt(total / double(count))));
        return m;
    }
private:
    void flush()
    {
        const auto rms = float(std::sqrt(energy / samples));
        const int bin = std::clamp(int(std::floor(dbFromGain(rms))) + 120, 0, 156);
        energies[size_t(bin)] += energy;
        windows[size_t(bin)] += std::uint64_t(samples);
        maxBin = std::max(maxBin, bin);
        energy = 0; samples = 0;
    }
    std::array<double, 157> energies {};
    std::array<std::uint64_t, 157> windows {};
    double energy = 0, sampleRate = 48000;
    int samples = 0, windowSize = 960, maxBin = 0;
    float peak = 0;
};

struct Suggestions
{
    std::array<float, laneCount> gainDb {};
    std::array<bool, laneCount> valid {};
    int activeLanes = 0;
};

inline Suggestions suggest(const std::array<Measurement, laneCount>& measurements,
                           const std::array<int, laneCount>& roles)
{
    Suggestions result;
    std::array<float, laneCount> anchors {};
    for (int i = 0; i < laneCount; ++i)
        if (measurements[size_t(i)].valid)
            anchors[size_t(result.activeLanes++)] = measurements[size_t(i)].rmsDb
                - roleOffsets[size_t(std::clamp(roles[size_t(i)], 0, roleCount - 1))];
    if (result.activeLanes < 2) return result;
    std::sort(anchors.begin(), anchors.begin() + result.activeLanes);
    const int middle = result.activeLanes / 2;
    const float reference = result.activeLanes % 2 ? anchors[size_t(middle)]
        : 0.5f * (anchors[size_t(middle - 1)] + anchors[size_t(middle)]);
    for (int i = 0; i < laneCount; ++i)
    {
        const auto index = size_t(i);
        if (!measurements[index].valid) continue;
        result.valid[index] = true;
        const float target = reference + roleOffsets[size_t(std::clamp(roles[index], 0, roleCount - 1))];
        // Bound both level changes and individual-track peak boosts.
        result.gainDb[index] = std::clamp(std::min(target - measurements[index].rmsDb,
                                                    -3.0f - measurements[index].peakDb), -12.0f, 12.0f);
    }
    return result;
}

class Smoother
{
public:
    void prepare(double rate, float milliseconds = 10.0f)
    { coefficient = std::exp(-1.0f / float(rate * milliseconds * 0.001)); }
    void reset(float value) { current = value; }
    float next(float target)
    {
        current = target + coefficient * (current - target);
        if (std::abs(current - target) < 1.0e-7f) current = target;
        return current;
    }
private:
    float current = 1, coefficient = 0;
};

class DuckEnvelope
{
public:
    void prepare(double rate)
    {
        attack = std::exp(-1.0f / float(rate * 0.001));
        release = std::exp(-1.0f / float(rate * 0.09));
        envelope = 0;
    }
    float next(float kickPeak, float amountDb)
    {
        const float target = std::clamp(kickPeak / gainFromDb(-12.0f), 0.0f, 1.0f);
        const float coefficient = target > envelope ? attack : release;
        envelope = target + coefficient * (envelope - target);
        return gainFromDb(-std::clamp(amountDb, 0.0f, 12.0f) * envelope);
    }
private:
    float envelope = 0, attack = 0, release = 0;
};

class PeakGuard
{
public:
    void prepare(double rate) { release = std::exp(-1.0f / float(rate * 0.08)); gain = 1; }
    float process(float& left, float& right, bool enabled)
    {
        left = clean(left); right = clean(right);
        if (!enabled) { gain = 1; return 0; }
        constexpr float ceiling = 0.89125094f; // -1 dBFS sample peak, not a true-peak limiter.
        const float peak = std::max(std::abs(left), std::abs(right));
        const float desired = peak > ceiling ? ceiling / peak : 1.0f;
        gain = desired < gain ? desired : std::min(desired, 1.0f + release * (gain - 1.0f));
        left *= gain; right *= gain;
        return -dbFromGain(gain);
    }
private:
    float gain = 1, release = 0;
};
}
