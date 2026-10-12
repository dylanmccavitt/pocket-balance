#include "BalanceEngine.h"
#include <cstdlib>
#include <iostream>
#include <limits>

void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void near(float actual, float expected, float tolerance, const char* message)
{ require(std::abs(actual - expected) <= tolerance, message); }

pocket::Measurement measure(double rate, float amplitude, bool sparse, bool antiPhase = false)
{
    pocket::Analyser analyser; analyser.prepare(rate);
    for (int n = 0; n < int(rate * 2); ++n)
    {
        const bool active = !sparse || (n % int(rate / 2) < int(rate * 0.1));
        const float s = active ? amplitude * std::sin(float(n) * 6.28318530718f * 100.0f / float(rate)) : 0;
        analyser.push(s, antiPhase ? -s : s);
    }
    return analyser.finish();
}
int main()
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto silence = measure(rate, 0, false);
        require(!silence.valid, "Silence cannot generate a suggestion");
        const auto steady = measure(rate, 0.5f, false);
        const auto sparse = measure(rate, 0.5f, true);
        const auto inverted = measure(rate, 0.5f, true, true);
        require(steady.valid && sparse.valid, "Normal signals must be measured");
        near(steady.rmsDb, -9.0309f, 0.02f, "RMS measurement");
        near(sparse.rmsDb, steady.rmsDb, 0.05f, "Silence cannot inflate sparse drum suggestions");
        near(inverted.rmsDb, sparse.rmsDb, 0.001f, "Antiphase stereo energy must not cancel");
        pocket::Analyser click; click.prepare(rate); click.push(1, 1);
        for (int n = 0; n < int(rate); ++n) click.push(0, 0);
        require(!click.finish().valid, "An isolated click is insufficient activity");
        pocket::PeakGuard guard; guard.prepare(rate);
        for (int n = 0; n < int(rate); ++n)
        {
            float left = n % 37 == 0 ? 16.0f : 0.5f, right = -left * 0.4f;
            guard.process(left, right, true);
            require(std::abs(left) <= 0.891251f && std::abs(right) <= 0.891251f, "Sample peak ceiling");
            near(right, -left * 0.4f, 0.00001f, "Guard preserves stereo ratio");
        }
        float l = 0.4f, r = -0.6f; guard.process(l, r, false);
        near(l, 0.4f, 0.00001f, "Guard off leaves normal audio unchanged");
        l = std::numeric_limits<float>::quiet_NaN(); r = std::numeric_limits<float>::infinity();
        guard.process(l, r, true);
        require(std::isfinite(l) && std::isfinite(r), "Invalid input is contained");
        pocket::DuckEnvelope duck; duck.prepare(rate);
        near(duck.next(1, 0), 1, 0.00001f, "Zero duck amount must be unity");
        float gain = 1;
        for (int n = 0; n < int(rate * 0.02); ++n) gain = duck.next(0.5f, 6);
        near(pocket::dbFromGain(gain), -6, 0.01f, "Ducking reaches selected amount");
        for (int n = 0; n < int(rate); ++n) gain = duck.next(0, 6);
        near(gain, 1, 0.0001f, "Ducking releases without a kick");
    }
    std::array<pocket::Measurement, pocket::laneCount> measurements {};
    std::array<int, pocket::laneCount> roles {};
    measurements[0] = { -12, -6, true };
    require(pocket::suggest(measurements, roles).activeLanes == 1, "One input is not a relative balance");
    measurements[1] = { -24, -18, true };
    const auto two = pocket::suggest(measurements, roles);
    near(two.gainDb[0], -6, 0.001f, "Louder same-role source is reduced");
    near(two.gainDb[1], 6, 0.001f, "Quieter same-role source is raised");
    roles[1] = 4; measurements[1] = { -22, -16, true };
    const auto musical = pocket::suggest(measurements, roles);
    near(musical.gainDb[0], 0, 0.001f, "Balanced kick unchanged");
    near(musical.gainDb[1], 0, 0.001f, "Hat stays ten dB below kick");
    measurements[1] = { -65, -59, true };
    const auto bounded = pocket::suggest(measurements, roles);
    for (float gain : bounded.gainDb) require(gain >= -12 && gain <= 12, "Suggestions bounded");
    require(!bounded.valid[2], "Silent lane unchanged");
    std::cout << "PASS: DSP measurement, roles, bounds, silence, stereo, ducking, guard at 44.1/48/96 kHz\n";
}
