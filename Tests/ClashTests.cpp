// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#include "ClashEngine.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
constexpr double pi = 3.14159265358979323846;
using Samples = std::array<float, pocket::laneCount>;

void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void near(double actual, double expected, double tolerance, const char* message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        std::cerr << "actual=" << actual << " expected=" << expected << '\n';
        require(false, message);
    }
}

float tone(int n, double rate, double frequency, float amplitude = 0.2f)
{ return amplitude * float(std::sin(2 * pi * frequency * n / rate)); }

enum class Pattern { silence, click, singleHit, twoHits, shortHits, repeated, sustained, alternating };

pocket::ClashReport analyse(double rate, Pattern pattern, double firstFrequency = 113.13708499,
                           double secondFrequency = 113.13708499, float firstAmplitude = 0.2f,
                           float secondAmplitude = 0.1f, bool antiPhase = false)
{
    pocket::ClashAnalyser analyser;
    analyser.prepare(rate);
    const int period = int(std::lround(rate * 0.4));
    const int hitLength = int(std::lround(rate * 0.1));
    for (int n = 0; n < int(rate * 1.6); ++n)
    {
        bool firstActive = false, secondActive = false;
        switch (pattern)
        {
            case Pattern::silence: break;
            case Pattern::click: firstActive = secondActive = n == 1; break;
            case Pattern::singleHit: firstActive = secondActive = n < hitLength; break;
            case Pattern::twoHits:
                firstActive = secondActive = n < period * 2 && n % period < hitLength; break;
            case Pattern::shortHits:
                firstActive = secondActive = n < period * 3 && n % period < int(rate * 0.02); break;
            case Pattern::repeated: firstActive = secondActive = n % period < hitLength; break;
            case Pattern::sustained: firstActive = secondActive = true; break;
            case Pattern::alternating:
                firstActive = n % period < hitLength;
                secondActive = n % period >= 2 * hitLength && n % period < 3 * hitLength;
                break;
        }
        Samples left {}, right {};
        left[0] = firstActive ? tone(n, rate, firstFrequency, firstAmplitude) : 0;
        left[1] = secondActive ? tone(n, rate, secondFrequency, secondAmplitude) : 0;
        if (pattern == Pattern::click && n == 1) left[0] = left[1] = 1;
        right = left;
        if (antiPhase) for (auto& sample : right) sample = -sample;
        analyser.push(left, right);
    }
    const auto result = analyser.finish();
    const auto repeatedRead = analyser.finish();
    require(result.count == repeatedRead.count, "Reading findings must not consume evidence");
    analyser.reset();
    require(analyser.finish().count == 0, "Reset must discard prior findings");
    return result;
}

double correctionLevel(double rate, double sourceFrequency, double triggerFrequency,
                       float depth = 2, bool antiPhase = false)
{
    pocket::ClashCorrection correction;
    correction.prepare(rate);
    correction.configure({ true, 0, 1, 1, depth });
    double inputEnergy = 0, outputEnergy = 0;
    for (int n = 0; n < int(rate * 0.8); ++n)
    {
        Samples left {}, right {};
        left[0] = tone(n, rate, sourceFrequency);
        left[1] = tone(n, rate, triggerFrequency);
        left[2] = 0.125f;
        right = left;
        if (antiPhase) for (auto& sample : right) sample = -sample;
        const auto originalL = left, originalR = right;
        correction.process(left, right);
        for (int i = 1; i < pocket::laneCount; ++i)
        {
            require(left[size_t(i)] == originalL[size_t(i)]
                    && right[size_t(i)] == originalR[size_t(i)], "Only the selected source may change");
        }
        if (antiPhase) near(right[0], -left[0], 1.0e-7, "Correction must preserve stereo antiphase");
        if (n > int(rate * 0.4))
        {
            inputEnergy += originalL[0] * originalL[0];
            outputEnergy += left[0] * left[0];
        }
        require(correction.reductionDb() >= 0 && correction.reductionDb() <= 3.00001f,
                "Displayed centre reduction must remain bounded");
    }
    return 10 * std::log10(outputEnergy / inputEnergy);
}

void testTransparency(double rate)
{
    pocket::ClashCorrection correction;
    correction.prepare(rate);
    Samples left {}, right {};
    for (int i = 0; i < pocket::laneCount; ++i)
    {
        left[size_t(i)] = float(i - 8) / 16;
        right[size_t(i)] = -left[size_t(i)] * 0.75f;
    }
    const auto originalL = left, originalR = right;
    correction.process(left, right);
    require(left == originalL && right == originalR, "Correction defaults to exact transparency");
    correction.configure({ true, 0, 1, 1, 3 });
    for (int n = 0; n < int(rate * 0.05); ++n)
    {
        left[0] = left[1] = tone(n, rate, 113.13708499);
        right = left;
        correction.process(left, right);
    }
    left = originalL; right = originalR;
    correction.process(left, right, true);
    require(left == originalL && right == originalR, "Bypass must be exact, including all other lanes");
    near(correction.reductionDb(), 0, 0, "Bypass clears the reduction indicator");
    correction.configure({ false, 0, 1, 1, 3 });
    correction.process(left, right);
    require(left == originalL && right == originalR, "Disabled correction is exactly transparent");
    for (const pocket::ClashConfig config : {
             pocket::ClashConfig { true, -1, 1, 1, 2 }, { true, 0, 16, 1, 2 },
             { true, 0, 0, 1, 2 }, { true, 0, 1, -1, 2 }, { true, 0, 1, 8, 2 },
             { true, 0, 1, 1, 0 }, { true, 0, 1, 1, -2 },
             { true, 0, 1, 1, std::numeric_limits<float>::quiet_NaN() } })
    {
        correction.configure(config);
        correction.process(left, right);
        require(left == originalL && right == originalR, "Invalid and zero-depth configurations are transparent");
    }
}

void testAttackAndRelease(double rate)
{
    pocket::ClashCorrection correction;
    correction.prepare(rate);
    const pocket::ClashConfig config { true, 0, 1, 1, 2 };
    correction.configure(config);
    float maximumStep = 0, previous = 0, initial = 0;
    for (int n = 0; n < int(rate * 0.3); ++n)
    {
        Samples left {}, right {};
        left[0] = left[1] = tone(n, rate, 113.13708499);
        right = left;
        // Host parameters may be supplied every block. Repeated configuration must not reset DSP.
        if (n % 64 == 0) correction.configure(config);
        correction.process(left, right);
        const float current = correction.reductionDb();
        maximumStep = std::max(maximumStep, std::abs(current - previous));
        previous = current;
        if (n == int(rate * 0.005)) initial = current;
    }
    require(initial > 0 && initial < 1.9f, "Attack starts gradually instead of switching the full cut");
    require(maximumStep < 0.025f, "Attack and depth smoothing limit per-sample gain steps");
    near(correction.reductionDb(), 2, 0.001, "A stable trigger reaches the requested centre reduction");
    float earlyRelease = 0;
    for (int n = 0; n < int(rate * 1.6); ++n)
    {
        Samples left {}, right {};
        left[0] = tone(n, rate, 113.13708499);
        right = left;
        correction.process(left, right);
        if (n == int(rate * 0.05)) earlyRelease = correction.reductionDb();
    }
    require(earlyRelease > 0.1f, "Release retains a smooth tail after the trigger stops");
    require(correction.reductionDb() < 0.001f, "Correction releases fully after trigger silence");
}

void testConfigurationTransitions(double rate)
{
    pocket::ClashCorrection correction;
    correction.prepare(rate);
    const pocket::ClashConfig original { true, 0, 1, 1, 3 };
    correction.configure(original);
    int sample = 0;
    auto render = [&](bool bypass = false)
    {
        Samples left {}, right {};
        left[0] = left[1] = tone(sample, rate, 113.13708499);
        left[2] = left[3] = tone(sample, rate, 452.54833996);
        left[4] = left[5] = tone(sample, rate, 226.27416998);
        right = left;
        for (auto& value : right) value = -value;
        const auto dry = left;
        correction.process(left, right, bypass);
        Samples residual {};
        for (int i = 0; i < pocket::laneCount; ++i)
        {
            near(right[size_t(i)], -left[size_t(i)], 1.0e-7,
                 "Configuration transitions preserve stereo linking");
            residual[size_t(i)] = left[size_t(i)] - dry[size_t(i)];
        }
        ++sample;
        return residual;
    };
    auto settleAtAudibleCut = [&]()
    {
        Samples residual {};
        const int start = sample;
        while (sample - start < int(rate * 0.3)) residual = render();
        for (int n = 0; n < int(rate * 0.02) && std::abs(residual[0]) < 0.05f; ++n) residual = render();
        require(std::abs(residual[0]) >= 0.05f, "Transition probe begins away from a waveform zero crossing");
        near(correction.reductionDb(), 3, 0.001, "Transition starts from an audible settled cut");
        return residual;
    };

    auto previous = settleAtAudibleCut();
    auto disabled = original;
    disabled.enabled = false;
    correction.configure(disabled);
    require(correction.reductionDb() > 2.9f, "Disabling schedules a fade instead of resetting the live cut");
    for (int n = 0; n < int(rate * 0.02); ++n)
    {
        // Repeated automation values cannot extend the five-millisecond fade.
        if (n % 64 == 0) correction.configure(disabled);
        const auto current = render();
        require(std::abs(current[0] - previous[0]) < 0.003f,
                "Disabling cannot introduce an abrupt correction waveform step");
        if (n == 0) require(std::abs(current[0]) > 0.045f, "The first disabled sample retains the fading old cut");
        if (n >= int(std::ceil(rate * 0.005)))
            require(current == Samples {}, "Disabled processing reaches exact transparency after five milliseconds");
        previous = current;
    }
    near(correction.reductionDb(), 0, 0, "A completed disable fade clears the indicator");

    correction.configure(original);
    previous = settleAtAudibleCut();
    pocket::ClashConfig desired { true, 2, 3, 3, 3 };
    correction.configure(desired);
    double newLaneEnergy = 0;
    for (int n = 0; n < int(rate * 0.3); ++n)
    {
        // A newer request during the old fade replaces the pending pair, without restarting the fade.
        if (n == int(rate * 0.001)) desired = { true, 4, 5, 2, 3 };
        correction.configure(desired);
        const auto current = render();
        for (int i : { 0, 2, 4 })
            require(std::abs(current[size_t(i)] - previous[size_t(i)]) < 0.007f,
                    "Pair and band changes cannot introduce an abrupt correction waveform step");
        require(current[2] == 0, "A superseded pending pair must never become active");
        if (n >= int(std::ceil(rate * 0.005)))
            require(current[0] == 0, "The old source becomes exactly transparent after the transition");
        if (n > int(rate * 0.2)) newLaneEnergy += current[4] * current[4];
        previous = current;
    }
    require(newLaneEnergy > 0.1, "The latest requested source and band become active");
    near(correction.reductionDb(), 3, 0.001, "The new correction settles at its requested depth");

    desired.enabled = false;
    correction.configure(desired);
    require(render(true) == Samples {}, "Host bypass is immediately transparent even during a pending fade");
    near(correction.reductionDb(), 0, 0, "Host bypass clears a pending transition immediately");
    require(render() == Samples {}, "The latest disabled configuration survives host bypass");
}
} // namespace

int main()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        require(analyse(rate, Pattern::silence).count == 0, "Silence cannot create clashes");
        require(analyse(rate, Pattern::click).count == 0, "One isolated click is insufficient evidence");
        require(analyse(rate, Pattern::singleHit).count == 0, "One short hit is insufficient evidence");
        require(analyse(rate, Pattern::twoHits).count == 0, "Two separated hits are insufficient evidence");
        require(analyse(rate, Pattern::shortHits).count == 0, "Repeated overlap must last at least 160 ms");
        require(analyse(rate, Pattern::alternating).count == 0, "Noncoactive sounds do not clash");
        require(analyse(rate, Pattern::sustained, 56.5685425, 7240.773439).count == 0,
                "Widely separated bands must not be reported as clashes");
        require(analyse(rate, Pattern::sustained, 113.137085, 113.137085, 0.2f, 0.001f).count == 0,
                "Inaudible lanes below the activity floor do not generate findings");
        const auto repeated = analyse(rate, Pattern::repeated);
        require(repeated.count == 1, "One repeated pair produces only its strongest-band finding");
        const auto& hit = repeated.findings[0];
        require(hit.first == 0 && hit.second == 1 && hit.band == 1,
                "Finding identifies the simultaneous pair and strongest band");
        require(hit.episodes >= 3 && !hit.sustained && hit.overlapSeconds >= 0.16f,
                "Repeated-hit evidence and duration are recorded");
        require(hit.reduceLane == 0 && hit.protectLane == 1, "Louder lane is the suggested reduction target");
        near(hit.levelDifferenceDb, 6.0206, 0.001, "Band-level difference is first minus second");
        require(hit.strength > 0 && hit.strength <= 1, "Evidence strength is bounded");
        const auto inverted = analyse(rate, Pattern::repeated, 113.137085, 113.137085, 0.2f, 0.1f, true);
        require(inverted.count == repeated.count, "Stereo polarity cannot cancel clash detection");
        near(inverted.findings[0].strength, hit.strength, 0.00001, "Antiphase has unchanged evidence strength");
        const auto equal = analyse(rate, Pattern::sustained, 113.137085, 113.137085, 0.2f, 0.2f);
        require(equal.count == 1 && equal.findings[0].sustained, "Sustained overlap qualifies without repeated hits");
        require(equal.findings[0].reduceLane == -1 && equal.findings[0].protectLane == -1,
                "Similar levels produce information without selecting a correction");
        const auto reversed = analyse(rate, Pattern::sustained, 113.137085, 113.137085, 0.1f, 0.2f);
        require(reversed.count == 1 && reversed.findings[0].reduceLane == 1
                && reversed.findings[0].protectLane == 0, "Directional recommendation follows the louder band");
        near(reversed.findings[0].levelDifferenceDb, -6.0206, 0.001, "Reversed difference has the correct sign");
        for (int band = 0; band < pocket::clashBandCount; ++band)
        {
            const double centre = std::sqrt(double(pocket::clashBandLow[size_t(band)])
                                            * pocket::clashBandHigh[size_t(band)]);
            const auto report = analyse(rate, Pattern::sustained, centre, centre);
            require(report.count == 1 && report.findings[0].band == band,
                    "Every octave band identifies its own centre frequency");
        }
        testTransparency(rate);
        testAttackAndRelease(rate);
        testConfigurationTransitions(rate);
        near(correctionLevel(rate, 113.13708499, 113.13708499), -2, 0.02,
             "Active correction reaches the requested band-centre depth");
        near(correctionLevel(rate, 113.13708499, 113.13708499, 3, true), -3, 0.02,
             "Maximum cut is 3 dB and remains stereo linked");
        near(correctionLevel(rate, 113.13708499, 113.13708499, 30), -3, 0.02,
             "Excessive depth requests are clamped to 3 dB");
        require(correctionLevel(rate, 1810.19336, 113.13708499) > -0.06,
                "Correction preserves distant source frequencies");
        near(correctionLevel(rate, 113.13708499, 1810.19336), 0, 0.00001,
             "A trigger outside the selected band cannot drive a cut");
    }

    // Dense sessions still have one strongest band per pair and a bounded, ranked report.
    pocket::ClashAnalyser dense;
    dense.prepare(48000);
    for (int n = 0; n < 60000; ++n)
    {
        Samples left {}, right {};
        for (int i = 0; i < pocket::laneCount; ++i)
            left[size_t(i)] = tone(n, 48000, 113.137085, 0.1f + float(i) * 0.005f);
        right = left;
        dense.push(left, right);
    }
    const auto denseReport = dense.finish();
    require(denseReport.count == pocket::maxClashFindings, "Dense reports are bounded to eight findings");
    for (int i = 0; i < denseReport.count; ++i)
    {
        const auto& finding = denseReport.findings[size_t(i)];
        if (i > 0) require(denseReport.findings[size_t(i - 1)].strength >= finding.strength,
                           "Findings are ranked by evidence strength");
        for (int j = 0; j < i; ++j)
            require(finding.first != denseReport.findings[size_t(j)].first
                    || finding.second != denseReport.findings[size_t(j)].second,
                    "Reports contain no duplicate pairs");
    }

    for (double rate : { 8000.0, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity() })
    {
        pocket::ClashAnalyser analyser;
        pocket::ClashCorrection correction;
        analyser.prepare(rate);
        correction.prepare(rate);
        correction.configure({ true, 0, 1, 7, 3 });
        for (int n = 0; n < 1000; ++n)
        {
            Samples left {}, right {};
            left[0] = std::numeric_limits<float>::quiet_NaN();
            left[1] = std::numeric_limits<float>::infinity();
            right[0] = -std::numeric_limits<float>::infinity();
            analyser.push(left, right);
            if (rate == 8000) left[0] = right[0] = 0.25f;
            correction.process(left, right);
            if (rate == 8000) near(left[0], 0.25, 0, "A band above Nyquist must disable correction");
            else require(std::isfinite(left[0]) && std::isfinite(right[0]),
                         "Active processing contains invalid source audio");
            require(std::isfinite(correction.reductionDb()), "Invalid rates cannot produce invalid detector output");
        }
        require(analyser.finish().count == 0, "Nonfinite samples cannot create evidence");
    }
    std::cout << "PASS: clash evidence, silence, repeats, sustained overlap, bands, ranking, stereo, "
                 "dynamic cut, bypass, smoothed configuration changes, invalid inputs at 44.1/48/96 kHz\n";
}
