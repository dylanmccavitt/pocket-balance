// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include "BalanceEngine.h"
#include <limits>

namespace pocket
{
inline constexpr int clashBandCount = 8;
inline constexpr int maxClashFindings = 8;
inline constexpr std::array<float, clashBandCount> clashBandLow {
    40, 80, 160, 320, 640, 1280, 2560, 5120 };
inline constexpr std::array<float, clashBandCount> clashBandHigh {
    80, 160, 320, 640, 1280, 2560, 5120, 10240 };

struct ClashFinding
{
    int first = -1, second = -1, band = 0, reduceLane = -1, protectLane = -1;
    float overlapSeconds = 0;
    // Signed band-level difference: first lane minus second lane, in dB.
    float levelDifferenceDb = 0, strength = 0;
    int episodes = 0;
    bool sustained = false;
};

struct ClashReport
{
    std::array<ClashFinding, maxClashFindings> findings {};
    int count = 0;
};

struct ClashConfig
{
    bool enabled = false;
    int source = 0, trigger = 1, band = 0;
    float depthDb = 2;
};

namespace clashDetail
{
constexpr double pi = 3.14159265358979323846;
constexpr double laneFloorPower = 1.0e-5;       // -50 dBFS RMS
constexpr double bandFloorPower = 3.16227766e-6; // -55 dBFS RMS
constexpr double minimumBandShare = 0.08;

inline double safeRate(double rate)
{
    return std::isfinite(rate) && rate >= 1000 && rate <= 768000 ? rate : 48000.0;
}

struct Coefficients
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

struct Biquad
{
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    float process(float input, const Coefficients& c)
    {
        const double output = c.b0 * input + z1;
        z1 = c.b1 * input - c.a1 * output + z2;
        z2 = c.b2 * input - c.a2 * output;
        // Avoid long denormal tails without changing audible output.
        if (std::abs(z1) < 1.0e-25) z1 = 0;
        if (std::abs(z2) < 1.0e-25) z2 = 0;
        return float(output);
    }
};

struct Band
{
    Coefficients analyse, cut;
    bool valid = false;
};

inline Band makeBand(int index, double rate)
{
    Band band;
    const double low = clashBandLow[size_t(index)], high = clashBandHigh[size_t(index)];
    // Omit incomplete bands at low sample rates. Never fold a band above Nyquist.
    if (high >= rate * 0.5) return band;
    const double centre = std::sqrt(low * high);
    const double omega = 2 * pi * centre / rate;
    const double alpha = std::sin(omega) / (2 * centre / (high - low));
    const double cosine = std::cos(omega);
    const double a0 = 1 + alpha;
    band.analyse = { alpha / a0, 0, -alpha / a0,
                     -2 * cosine / a0, (1 - alpha) / a0 };
    // RBJ peaking EQ, fixed -3 dB at the geometric band centre.
    const double amplitude = std::pow(10.0, -3.0 / 40.0);
    const double cutA0 = 1 + alpha / amplitude;
    band.cut = { (1 + alpha * amplitude) / cutA0, -2 * cosine / cutA0,
                 (1 - alpha * amplitude) / cutA0, -2 * cosine / cutA0,
                 (1 - alpha / amplitude) / cutA0 };
    band.valid = true;
    return band;
}

inline double stereoPower(float left, float right)
{
    return 0.5 * (double(left) * left + double(right) * right);
}
} // namespace clashDetail

// Synchronous 20 ms stereo-energy windows establish repeated coactivity.
// Findings describe potential masking, not a perceptual verdict about a mix.
class ClashAnalyser
{
public:
    void prepare(double rate)
    {
        sampleRate = clashDetail::safeRate(rate);
        frameSize = std::max(1, int(std::lround(sampleRate * 0.02)));
        frameSeconds = double(frameSize) / sampleRate;
        gapFramesNeeded = std::max(1, int(std::ceil(0.06 / frameSeconds - 1.0e-9)));
        for (int b = 0; b < clashBandCount; ++b)
            bands[size_t(b)] = clashDetail::makeBand(b, sampleRate);
        reset();
    }

    void reset()
    {
        laneEnergy.fill(0);
        for (auto& lane : bandEnergy) lane.fill(0);
        for (auto& lane : filters)
            for (auto& band : lane)
                for (auto& channel : band) channel.reset();
        for (auto& pair : pairs)
            for (auto& evidence : pair)
            {
                evidence = {};
                evidence.gap = std::uint64_t(gapFramesNeeded);
            }
        frameSamples = 0;
    }

    void push(const std::array<float, laneCount>& left,
              const std::array<float, laneCount>& right)
    {
        for (int i = 0; i < laneCount; ++i)
        {
            const auto lane = size_t(i);
            const float l = clean(left[lane]), r = clean(right[lane]);
            laneEnergy[lane] += clashDetail::stereoPower(l, r);
            for (int b = 0; b < clashBandCount; ++b)
            {
                const auto band = size_t(b);
                if (!bands[band].valid) continue;
                const float filteredL = filters[lane][band][0].process(l, bands[band].analyse);
                const float filteredR = filters[lane][band][1].process(r, bands[band].analyse);
                bandEnergy[lane][band] += clashDetail::stereoPower(filteredL, filteredR);
            }
        }
        if (++frameSamples >= frameSize) flush();
    }

    ClashReport finish() const
    {
        ClashReport report;
        int pairIndex = 0;
        for (int first = 0; first < laneCount; ++first)
            for (int second = first + 1; second < laneCount; ++second, ++pairIndex)
            {
                ClashFinding best;
                for (int b = 0; b < clashBandCount; ++b)
                {
                    const auto& evidence = pairs[size_t(pairIndex)][size_t(b)];
                    const double overlap = evidence.frames * frameSeconds;
                    const bool sustained = evidence.longestRun * frameSeconds + 1.0e-9 >= 1.0;
                    if (overlap + 1.0e-9 < 0.16 || (evidence.episodes < 3 && !sustained)) continue;
                    ClashFinding finding;
                    finding.first = first;
                    finding.second = second;
                    finding.band = b;
                    finding.overlapSeconds = float(overlap);
                    finding.episodes = int(std::min(evidence.episodes,
                        std::uint64_t(std::numeric_limits<int>::max())));
                    finding.sustained = sustained;
                    finding.levelDifferenceDb = float(10 * std::log10(evidence.firstEnergy
                                                                       / evidence.secondEnergy));
                    finding.strength = float(std::clamp(evidence.concentration / double(evidence.frames)
                                                        * std::min(1.0, overlap / 1.5), 0.0, 1.0));
                    // Similar levels are informational. There is no obvious lane to reduce.
                    if (std::abs(finding.levelDifferenceDb) >= 3.0f)
                    {
                        finding.reduceLane = finding.levelDifferenceDb > 0 ? first : second;
                        finding.protectLane = finding.levelDifferenceDb > 0 ? second : first;
                    }
                    if (best.first < 0 || finding.strength > best.strength) best = finding;
                }
                if (best.first < 0) continue;
                int insertion = 0;
                while (insertion < report.count
                       && report.findings[size_t(insertion)].strength >= best.strength) ++insertion;
                if (insertion >= maxClashFindings) continue;
                report.count = std::min(report.count + 1, maxClashFindings);
                for (int i = report.count - 1; i > insertion; --i)
                    report.findings[size_t(i)] = report.findings[size_t(i - 1)];
                report.findings[size_t(insertion)] = best;
            }
        // Partial windows are intentionally excluded; they cannot add a complete evidence frame.
        return report;
    }

private:
    struct Evidence
    {
        std::uint64_t frames = 0, run = 0, longestRun = 0, gap = 3, episodes = 0;
        double firstEnergy = 0, secondEnergy = 0, concentration = 0;
    };

    void flush()
    {
        std::array<std::array<bool, clashBandCount>, laneCount> active {};
        for (int i = 0; i < laneCount; ++i)
            for (int b = 0; b < clashBandCount; ++b)
            {
                const auto lane = size_t(i), band = size_t(b);
                active[lane][band] = bands[band].valid
                    && laneEnergy[lane] / frameSize > clashDetail::laneFloorPower
                    && bandEnergy[lane][band] / frameSize > clashDetail::bandFloorPower
                    && bandEnergy[lane][band] >= clashDetail::minimumBandShare * laneEnergy[lane];
            }
        int pairIndex = 0;
        for (int first = 0; first < laneCount; ++first)
            for (int second = first + 1; second < laneCount; ++second, ++pairIndex)
                for (int b = 0; b < clashBandCount; ++b)
                {
                    const auto a = size_t(first), c = size_t(second), band = size_t(b);
                    auto& evidence = pairs[size_t(pairIndex)][band];
                    if (active[a][band] && active[c][band])
                    {
                        if (evidence.gap >= std::uint64_t(gapFramesNeeded)) ++evidence.episodes;
                        evidence.gap = 0;
                        ++evidence.frames;
                        evidence.longestRun = std::max(evidence.longestRun, ++evidence.run);
                        evidence.firstEnergy += bandEnergy[a][band];
                        evidence.secondEnergy += bandEnergy[c][band];
                        evidence.concentration += std::min({ 1.0,
                            bandEnergy[a][band] / laneEnergy[a], bandEnergy[c][band] / laneEnergy[c] });
                    }
                    else
                    {
                        evidence.run = 0;
                        // Only the 60 ms distinction is needed; prevent idle-counter overflow.
                        evidence.gap = std::min(evidence.gap + 1, std::uint64_t(gapFramesNeeded));
                    }
                }
        laneEnergy.fill(0);
        for (auto& lane : bandEnergy) lane.fill(0);
        frameSamples = 0;
    }

    static constexpr int pairCount = laneCount * (laneCount - 1) / 2;
    std::array<clashDetail::Band, clashBandCount> bands {};
    std::array<std::array<std::array<clashDetail::Biquad, 2>, clashBandCount>, laneCount> filters {};
    std::array<double, laneCount> laneEnergy {};
    std::array<std::array<double, clashBandCount>, laneCount> bandEnergy {};
    std::array<std::array<Evidence, clashBandCount>, pairCount> pairs {};
    double sampleRate = 48000, frameSeconds = 0.02;
    int frameSize = 960, frameSamples = 0, gapFramesNeeded = 3;
};

// One optional stereo-linked correction. Source and protected trigger are separate lanes.
// The detector gates on the protected lane's selected-band energy. It does not infer masking.
class ClashCorrection
{
public:
    void prepare(double rate)
    {
        sampleRate = clashDetail::safeRate(rate);
        attack = std::exp(-1.0 / (sampleRate * 0.005));
        release = std::exp(-1.0 / (sampleRate * 0.12));
        detector = std::exp(-1.0 / (sampleRate * 0.01));
        for (int b = 0; b < clashBandCount; ++b)
            bands[size_t(b)] = clashDetail::makeBand(b, sampleRate);
        const auto previous = config;
        config = {};
        configure(previous);
        reset();
    }

    void configure(const ClashConfig& requested)
    {
        ClashConfig next = requested;
        next.depthDb = std::isfinite(next.depthDb) ? std::clamp(next.depthDb, 0.0f, 3.0f) : 0.0f;
        const bool valid = next.source >= 0 && next.source < laneCount
            && next.trigger >= 0 && next.trigger < laneCount && next.source != next.trigger
            && next.band >= 0 && next.band < clashBandCount;
        next.enabled = next.enabled && valid && next.depthDb > 0;
        if (next.enabled) next.enabled = bands[size_t(next.band)].valid;
        const bool changed = next.source != config.source || next.trigger != config.trigger
            || next.band != config.band || next.enabled != config.enabled;
        config = next;
        targetMix = (1.0 - std::pow(10.0, -double(config.depthDb) / 20.0)) / (1.0 - fullCutGain);
        if (changed) reset();
    }

    void process(std::array<float, laneCount>& left, std::array<float, laneCount>& right,
                 bool bypass = false)
    {
        if (!config.enabled || bypass)
        {
            if (!suspended) reset();
            suspended = true;
            return; // True transparency: even unrelated lanes are never touched.
        }
        suspended = false;
        const auto source = size_t(config.source), trigger = size_t(config.trigger);
        const auto& band = bands[size_t(config.band)];
        const float triggerL = clean(left[trigger]), triggerR = clean(right[trigger]);
        const float filteredL = triggerFilters[0].process(triggerL, band.analyse);
        const float filteredR = triggerFilters[1].process(triggerR, band.analyse);
        lanePower = detector * lanePower + (1 - detector) * clashDetail::stereoPower(triggerL, triggerR);
        bandPower = detector * bandPower + (1 - detector) * clashDetail::stereoPower(filteredL, filteredR);
        const bool active = lanePower > clashDetail::laneFloorPower
            && bandPower > clashDetail::bandFloorPower
            && bandPower >= clashDetail::minimumBandShare * lanePower;
        const double target = active ? 1.0 : 0.0;
        const double coefficient = target > envelope ? attack : release;
        envelope = target + coefficient * (envelope - target);
        mix = targetMix + attack * (mix - targetMix);
        const float wet = float(std::clamp(mix * envelope, 0.0, 1.0));
        const float dryL = clean(left[source]), dryR = clean(right[source]);
        const float cutL = sourceFilters[0].process(dryL, band.cut);
        const float cutR = sourceFilters[1].process(dryR, band.cut);
        left[source] = dryL + wet * (cutL - dryL);
        right[source] = dryR + wet * (cutR - dryR);
        lastReduction = float(-20 * std::log10(1.0 - wet * (1.0 - fullCutGain)));
    }

    // Estimated gain reduction at the band's centre, not a full-band loudness measurement.
    float reductionDb() const { return lastReduction; }

private:
    void reset()
    {
        for (auto& channel : triggerFilters) channel.reset();
        for (auto& channel : sourceFilters) channel.reset();
        lanePower = bandPower = envelope = mix = 0;
        lastReduction = 0;
    }
    static constexpr double fullCutGain = 0.7079457843841379;
    ClashConfig config;
    std::array<clashDetail::Band, clashBandCount> bands {};
    std::array<clashDetail::Biquad, 2> triggerFilters {}, sourceFilters {};
    double sampleRate = 48000, attack = 0, release = 0, detector = 0;
    double lanePower = 0, bandPower = 0, envelope = 0, mix = 0, targetMix = 0;
    float lastReduction = 0;
    bool suspended = true;
};
} // namespace pocket
