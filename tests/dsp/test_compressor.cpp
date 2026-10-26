#include "measure.hpp"
#include "osp/dsp/Compressor.h"
#include "rt_checker.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <tuple>
#include <vector>
#include <xmmintrin.h>

using osp::dsp::CompMode;
using osp::dsp::Compressor;

namespace {

// Renders `in` in blocks of `blockSize` under the RT checker (block size 0 adds a no-op call before every
// block).
std::vector<float> render(Compressor& comp, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            if (blockSize == 0)
                comp.process(in.data() + pos, out.data() + pos, 0);
            comp.process(in.data() + pos, out.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

Compressor make(double fs, CompMode mode, float thr, float ratio, float knee, float attackMs, float releaseMs)
{
    Compressor c;
    c.setMode(mode);
    c.setThresholdDb(thr);
    c.setRatio(ratio);
    c.setKneeDb(knee);
    c.setAttackMs(attackMs);
    c.setReleaseMs(releaseMs);
    c.setAutoMakeup(false);
    c.prepare(fs, 2048);
    return c;
}

std::vector<float> dc(double level, int n)
{
    return std::vector<float>(static_cast<std::size_t>(n), static_cast<float>(level));
}

std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

double theoryDb(CompMode mode, double xDb, float thr, float ratio, float knee)
{
    if (mode != CompMode::dual)
        return Compressor::staticGainReductionDb(static_cast<float>(xDb), thr, ratio, knee);
    const float r = std::sqrt(ratio);
    const float g1 = Compressor::staticGainReductionDb(static_cast<float>(xDb), thr, r, knee);
    return g1 + Compressor::staticGainReductionDb(static_cast<float>(xDb) - g1, thr, r, knee);
}

} // namespace

TEST_CASE("Compressor: static curve matches the closed form and is continuous at the knee edges", "[dsp][comp]")
{
    // Hard knee: 0 below, (1 - 1/R)(x - T) above.
    CHECK(Compressor::staticGainReductionDb(-30.0f, -20.0f, 4.0f, 0.0f) == 0.0f);
    CHECK(std::abs(Compressor::staticGainReductionDb(0.0f, -20.0f, 4.0f, 0.0f) - 15.0f) < 1e-5f);
    // Soft knee: (1 - 1/R)(W/2)^2 / (2W) = (1 - 1/R) W / 8 at x = T.
    CHECK(std::abs(Compressor::staticGainReductionDb(-20.0f, -20.0f, 4.0f, 6.0f) - 0.5625f) < 1e-5f);
    for (float edge : {-23.0f, -17.0f})
        CHECK(std::abs(Compressor::staticGainReductionDb(edge - 1e-3f, -20.0f, 4.0f, 6.0f) -
                       Compressor::staticGainReductionDb(edge + 1e-3f, -20.0f, 4.0f, 6.0f)) < 1e-2f);
}

TEST_CASE("Compressor: measured static curve within 0.5 dB of theory (DC levels)", "[dsp][comp]")
{
    const CompMode mode = GENERATE(CompMode::vca, CompMode::opto, CompMode::dual);
    const auto [thr, ratio, knee] = GENERATE(std::tuple{-20.0f, 4.0f, 6.0f}, std::tuple{-30.0f, 20.0f, 0.0f},
                                             std::tuple{-10.0f, 1.5f, 12.0f}, std::tuple{-50.0f, 8.0f, 3.0f});
    CAPTURE(static_cast<int>(mode), thr, ratio, knee);
    for (int level = -60; level <= 0; level += 3) {
        auto comp = make(48000.0, mode, thr, ratio, knee, 1.0f, 20.0f);
        const double x = std::pow(10.0, level / 20.0);
        const auto out = render(comp, dc(x, 48000), 512);
        const double measured = -20.0 * std::log10(out.back() / x);
        CAPTURE(level, measured);
        REQUIRE(std::abs(measured - theoryDb(mode, level, thr, ratio, knee)) <= 0.5);
        if (mode == CompMode::dual && knee == 0.0f) // sqrt(R) x sqrt(R) = R outside the knee
            REQUIRE(std::abs(measured - theoryDb(CompMode::vca, level, thr, ratio, knee)) <= 0.01);
    }
}

TEST_CASE("Compressor: 1 kHz sine with a slow release sits on the static curve at its peak", "[dsp][comp]")
{
    auto comp = make(48000.0, CompMode::vca, -24.0f, 4.0f, 0.0f, 0.1f, 1000.0f);
    std::vector<float> x(48000);
    for (std::size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(n) / 48000.0));
    render(comp, x, 64);
    const double expected = Compressor::staticGainReductionDb(-6.02f, -24.0f, 4.0f, 0.0f);
    CAPTURE(comp.getGainReductionDb(), expected);
    REQUIRE(std::abs(comp.getGainReductionDb() - expected) <= 0.5);
}

TEST_CASE("Compressor: attack and release are 63 % time constants", "[dsp][comp]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const float tau = GENERATE(1.0f, 10.0f, 100.0f);
    CAPTURE(fs, tau);
    // T = -20, R = 4, hard knee: a 0 dBFS step asks for 15 dB.
    auto comp = make(fs, CompMode::vca, -20.0f, 4.0f, 0.0f, tau, 5.0f * tau + 20.0f);
    const int n = static_cast<int>(std::lround(tau * 0.001 * fs));
    render(comp, dc(1.0, n), 1);
    REQUIRE(std::abs(comp.getGainReductionDb() / 15.0f - 0.632f) <= 0.005f);

    const float release = GENERATE(20.0f, 150.0f, 1000.0f);
    CAPTURE(release);
    comp = make(fs, CompMode::vca, -20.0f, 4.0f, 0.0f, 0.1f, release);
    render(comp, dc(1.0, static_cast<int>(0.05 * fs)), 64);
    const float start = comp.getGainReductionDb();
    render(comp, dc(0.0, static_cast<int>(std::lround(release * 0.001 * fs))), 1);
    REQUIRE(std::abs(comp.getGainReductionDb() / start - 0.368f) <= 0.005f);
}

TEST_CASE("Compressor: opto release is two-stage and program-dependent", "[dsp][comp]")
{
    const double fs = 48000.0;
    // GR left 250 ms (5 x release) after a burst of `burstMs`, relative to the GR at the end of the burst.
    auto left = [&](CompMode mode, double burstMs) {
        auto comp = make(fs, mode, -30.0f, 10.0f, 0.0f, 5.0f, 50.0f);
        render(comp, dc(1.0, static_cast<int>(burstMs * 0.001 * fs)), 64);
        const float start = comp.getGainReductionDb();
        render(comp, dc(0.0, static_cast<int>(0.25 * fs)), 64);
        return comp.getGainReductionDb() / start;
    };
    const float vca = left(CompMode::vca, 2000.0), optoShort = left(CompMode::opto, 10.0),
                optoLong = left(CompMode::opto, 2000.0);
    CAPTURE(vca, optoShort, optoLong);
    REQUIRE(vca < 0.01f);       // e^-5: single stage
    REQUIRE(optoShort < 0.05f); // a short peak barely charges the slow stage
    REQUIRE(optoLong > 0.2f);   // sustained compression leaves the slow tail (theory 0.5 e^-0.625 = 0.27)
    REQUIRE(optoLong < 0.35f);
}

TEST_CASE("Compressor: dual mode attacks more slowly than vca (stage 2 has 4x attack)", "[dsp][comp]")
{
    auto grAfter = [](CompMode mode) {
        auto comp = make(48000.0, mode, -20.0f, 4.0f, 0.0f, 10.0f, 100.0f);
        render(comp, dc(1.0, 480), 64);
        return comp.getGainReductionDb();
    };
    REQUIRE(grAfter(CompMode::dual) < grAfter(CompMode::vca));
}

TEST_CASE("Compressor: blend is phase-coherent (identity nulls) and hard bypass is exact", "[dsp][comp]")
{
    const auto in = noise(9600, 7u);
    const CompMode mode = GENERATE(CompMode::vca, CompMode::opto, CompMode::dual);
    const float blend = GENERATE(0.0f, 50.0f, 100.0f);
    CAPTURE(static_cast<int>(mode), blend);

    // Ratio 1 with auto makeup on (which is then 0 dB): wet == dry, so every blend nulls.
    auto identity = make(48000.0, mode, -40.0f, 1.0f, 6.0f, 1.0f, 50.0f);
    identity.setAutoMakeup(true);
    identity.setBlendPct(blend);
    identity.reset();
    REQUIRE(render(identity, in, 64) == in);

    // Blend 0 is the dry signal however hard the comp works.
    auto heavy = make(48000.0, mode, -50.0f, 20.0f, 0.0f, 0.1f, 20.0f);
    heavy.setBlendPct(0.0f);
    heavy.reset();
    REQUIRE(render(heavy, in, 64) == in);

    heavy.setBlendPct(blend);
    heavy.setBypassed(true);
    REQUIRE(render(heavy, in, 127) == in);
}

TEST_CASE("Compressor: rate x block-size matrix, reset() and pre-prepare", "[dsp][comp]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto in = noise(6000, 3u);
    auto ref = make(fs, CompMode::opto, -30.0f, 6.0f, 6.0f, 3.0f, 80.0f);
    const auto expected = render(ref, in, 1);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(fs, block);
        auto comp = make(fs, CompMode::opto, -30.0f, 6.0f, 6.0f, 3.0f, 80.0f);
        REQUIRE(render(comp, in, block) == expected);
        comp.reset();
        REQUIRE(render(comp, in, block) == expected);
    }

    Compressor unprepared;
    std::vector<float> out(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(in.data(), out.data(), 64); }));
    REQUIRE(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Compressor: 1000 random parameter vectors, mode switches, NaN/subnormal scan", "[dsp][comp][rt]")
{
    Compressor comp;
    comp.prepare(48000.0, 512);
    const auto in = noise(512, 11u);
    std::vector<float> out(in.size());
    std::size_t bad = 0;
    std::uint32_t s = 99u;
    auto uni = [&](float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>(s >> 8) / 16777216.0f;
    };
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            comp.setThresholdDb(uni(-50.0f, 0.0f));
            comp.setRatio(uni(1.0f, 20.0f));
            comp.setKneeDb(uni(0.0f, 12.0f));
            comp.setAttackMs(uni(0.1f, 100.0f));
            comp.setReleaseMs(uni(20.0f, 1000.0f));
            comp.setBlendPct(uni(0.0f, 100.0f));
            comp.setMakeupDb(uni(0.0f, 24.0f));
            comp.setAutoMakeup(uni(0.0f, 1.0f) > 0.5f);
            comp.setMode(static_cast<CompMode>(static_cast<int>(uni(0.0f, 2.999f))));
            comp.process(in.data(), out.data(), v % 2 == 0 ? 512 : 127);
            const auto scan = osp::rt::test::scanOutput(out);
            bad += scan.nonFinite + scan.subnormal;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    REQUIRE(bad == 0u);

    // Impulse, then 10 s of silence: the detectors decay to exactly 0 (opto slow stage: 8 x 50 ms) without
    // subnormals.
    comp.setMode(CompMode::opto);
    comp.setReleaseMs(50.0f);

    std::vector<float> silence(480000, 0.0f);
    silence[0] = 1.0f;
    const auto tail = render(comp, silence, 512);
    REQUIRE(comp.getGainReductionDb() == 0.0f);
    REQUIRE(tail.back() == 0.0f);
}

namespace {

// Identity compressor (ratio 1, auto makeup off), so only the stage under test shapes the signal.
Compressor identity(double fs)
{
    return make(fs, CompMode::vca, -20.0f, 1.0f, 0.0f, 10.0f, 150.0f);
}

// |H| of the analog tilt prototype at the frequency the prewarped bilinear transform maps `hz` to.
double tiltTheoryDb(double hz, double fs, float knob)
{
    const double g = std::pow(10.0, (knob - 5.0) * 1.2 / 20.0);
    const double w = std::tan(3.14159265358979323846 * hz / fs) / std::tan(3.14159265358979323846 * 1000.0 / fs);
    return 20.0 * std::log10(std::abs(std::complex<double>(1.0, g * w) / std::complex<double>(g, w)));
}

// First block boundary at or after the middle of `n` samples: where a per-block parameter change lands.
int deliverySample(int n, int block)
{
    return (n / 2 + block - 1) / block * block;
}

// Renders `in` with `change()` applied before the block that starts at `at`.
template <typename Change>
std::vector<float> renderWithStep(Compressor& comp, const std::vector<float>& in, int block, int at, Change&& change)
{
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block) {
            if (pos == at)
                change();
            comp.process(in.data() + pos, out.data() + pos, std::min(block, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}

} // namespace

TEST_CASE("Compressor: tone is a +-6 dB tilt around 1 kHz", "[dsp][comp][tone]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const float knob = GENERATE(0.0f, 2.5f, 7.5f, 10.0f);
    CAPTURE(fs, knob);
    for (double nominal : {30.0, 1000.0, 5000.0, 18000.0}) {
        const auto tone = osp::measure::coherentTone(nominal, fs);
        const std::size_t len = osp::measure::kWarmupSamples + osp::measure::kCoherentFftSize;
        const auto in = osp::measure::makeTone(tone, fs, -12.0, len);
        auto comp = identity(fs);
        comp.setTone(knob);
        comp.reset();
        const auto out = render(comp, in, 512);
        std::vector<double> tailIn(in.end() - osp::measure::kCoherentFftSize, in.end());
        std::vector<double> tailOut(out.end() - osp::measure::kCoherentFftSize, out.end());
        const double measured = osp::measure::toDb(std::abs(osp::measure::realFft(tailOut)[tone.bin]) /
                                                   std::abs(osp::measure::realFft(tailIn)[tone.bin]));
        CAPTURE(tone.frequency, measured);
        REQUIRE(std::abs(measured - tiltTheoryDb(tone.frequency, fs, knob)) <= 0.05);
        if (knob == 10.0f && nominal == 30.0)
            REQUIRE(std::abs(measured + 6.0) <= 0.3);
        if (knob == 10.0f && nominal == 18000.0 && fs >= 48000.0)
            REQUIRE(std::abs(measured - 6.0) <= 0.3);
    }
}

TEST_CASE("Compressor: makeup, blend and tone steps leave <= -60 dBc residual (REQ-TEST-003)", "[dsp][comp][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const int block = GENERATE(37, 64, 512);
    const bool up = GENERATE(true, false);
    CAPTURE(fs, block, up);
    const auto tone = osp::measure::coherentTone(1000.0, fs);
    const int n = static_cast<int>(0.4 * fs);
    const auto in = osp::measure::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const int at = deliverySample(n, block);
    const int len = static_cast<int>(std::lround(0.020 * fs));
    auto ramp = [&](int i, double from, double to) {
        return i < at ? from : from + (to - from) * std::min(1.0, static_cast<double>(i - at + 1) / len);
    };
    std::vector<double> ref(static_cast<std::size_t>(n));

    // Makeup 0 <-> 24 dB: linear ramp of the linear gain over 20 ms.
    const double m0 = up ? 1.0 : std::pow(10.0, 1.2), m1 = up ? std::pow(10.0, 1.2) : 1.0;
    auto comp = identity(fs);
    comp.setMakeupDb(up ? 0.0f : 24.0f);
    comp.reset();
    auto out = renderWithStep(comp, in, block, at, [&] { comp.setMakeupDb(up ? 24.0f : 0.0f); });
    for (int i = 0; i < n; ++i)
        ref[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * ramp(i, m0, m1);
    const double rMakeup = osp::measure::residualDbc(out, ref);

    // Blend 0 <-> 100 % with a +12 dB wet path: out = x ((1 - b) + b m).
    const double m = std::pow(10.0, 0.6);
    comp = identity(fs);
    comp.setMakeupDb(12.0f);
    comp.setBlendPct(up ? 0.0f : 100.0f);
    comp.reset();
    out = renderWithStep(comp, in, block, at, [&] { comp.setBlendPct(up ? 100.0f : 0.0f); });
    for (int i = 0; i < n; ++i) {
        const double b = ramp(i, up ? 0.0 : 1.0, up ? 1.0 : 0.0);
        ref[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * ((1.0 - b) + b * m);
    }
    const double rBlend = osp::measure::residualDbc(out, ref);

    // Tone 0 <-> 10: one-pole 20 ms on the knob, reference tilt recomputed every sample in double.
    comp = identity(fs);
    comp.setTone(up ? 0.0f : 10.0f);
    comp.reset();
    out = renderWithStep(comp, in, block, at, [&] { comp.setTone(up ? 10.0f : 0.0f); });
    const double a = std::exp(-1.0 / (0.020 * fs)), k = 1.0 / std::tan(3.14159265358979323846 * 1000.0 / fs);
    const double from = up ? 0.0 : 10.0, to = up ? 10.0 : 0.0;
    double x1 = 0.0, y1 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double knob = i < at ? from : to + (from - to) * std::pow(a, i - at + 1);
        const double g = std::pow(10.0, (knob - 5.0) * 1.2 / 20.0), a0 = k + g;
        const double x = in[static_cast<std::size_t>(i)];
        const double y = ((g * k + 1.0) * x + (1.0 - g * k) * x1 - (g - k) * y1) / a0;
        x1 = x;
        y1 = y;
        ref[static_cast<std::size_t>(i)] = y;
    }
    const double rTone = osp::measure::residualDbc(out, ref);

    CAPTURE(rMakeup, rBlend, rTone);
    REQUIRE(rMakeup <= -60.0);
    REQUIRE(rBlend <= -60.0);
    REQUIRE(rTone <= -60.0);
    if (block == 64 && fs == 48000.0)
        WARN("REQ-TEST-003 comp residual dBc (" << (up ? "up" : "down") << "): makeup " << rMakeup << ", blend "
                                                << rBlend << ", tone " << rTone);
}

TEST_CASE("Compressor: auto makeup brings a -12 dBFS DI back to about -12 dBFS", "[dsp][comp]")
{
    // Auto makeup is the static GR at -12 dBFS. On a sine the peak detector sags between peaks, so the dynamic GR
    // is a little lower and the output a little hot, more so with slower attacks (measured: 1 ms -11.82,
    // 10 ms -11.26, 30 ms -10.64 dBFS in vca). Defaults otherwise (T -20, R 4, knee 6, release 150).
    auto peakOut = [](float attackMs) {
        Compressor comp;
        comp.setMode(CompMode::vca);
        comp.setAttackMs(attackMs);
        comp.prepare(48000.0, 64);
        const auto tone = osp::measure::coherentTone(997.0, 48000.0);
        const auto out = render(comp, osp::measure::makeTone(tone, 48000.0, -12.0, 48000), 64);
        float peak = 0.0f;
        for (std::size_t i = out.size() - 24000; i < out.size(); ++i)
            peak = std::max(peak, std::abs(out[i]));
        return 20.0 * std::log10(peak);
    };
    const double fast = peakOut(1.0f), byDefault = peakOut(10.0f);
    CAPTURE(fast, byDefault);
    REQUIRE(std::abs(fast + 12.0) <= 0.5);
    REQUIRE(byDefault >= -12.0);
    REQUIRE(byDefault <= -11.0);
}

TEST_CASE("Compressor: mode and auto-makeup switches are click-free (xf 10)", "[dsp][comp]")
{
    // Heavy GR (T -40, R 20: about 26.6 dB of auto makeup at -12 dBFS), switch mid-signal.
    const double fs = 48000.0;
    const auto tone = osp::measure::coherentTone(997.0, fs);
    const auto in = osp::measure::makeTone(tone, fs, -12.0, 48000);
    auto maxStep = [](const std::vector<float>& y, int from, int to) {
        float m = 0.0f;
        for (int i = from + 1; i < to; ++i)
            m = std::max(m, std::abs(y[static_cast<std::size_t>(i)] - y[static_cast<std::size_t>(i - 1)]));
        return m;
    };
    // Switch at 8 phases across one period (48 samples) with 1-sample blocks, so a hard switch cannot hide in a
    // zero crossing.
    for (int sw = 0; sw < 4; ++sw)
        for (int at = 24000; at < 24048; at += 6) {
            CAPTURE(sw, at);
            auto comp = make(fs, CompMode::vca, -40.0f, 20.0f, 0.0f, 10.0f, 150.0f);
            comp.setAutoMakeup(true);
            comp.reset();
            const auto out = renderWithStep(comp, in, 1, at, [&] {
                if (sw == 0)
                    comp.setAutoMakeup(false);
                else
                    comp.setMode(sw == 1 ? CompMode::opto : sw == 2 ? CompMode::dual : CompMode::vca);
            });
            // Steady state on both sides: the new mode may settle at a slightly different level (dual is louder).
            const float steady = std::max(maxStep(out, at - 2400, at), maxStep(out, at + 2400, at + 4800));
            const float after = maxStep(out, at - 1, at + 1440); // the 10 ms fade plus margin
            CAPTURE(steady, after);
            // A hard switch would add up to |x| * g * (auto gain - 1), about 0.24 here; the ramp adds ~5e-4 per sample.
            REQUIRE(after <= steady + 1.0e-3f);
        }
}

TEST_CASE("Compressor: tilt tail never goes subnormal, even with FTZ off", "[dsp][comp][tone]")
{
    REQUIRE((_mm_getcsr() & 0x8040u) == 0u); // FTZ and DAZ are off outside the RT harness
    for (float knob : {0.0f, 10.0f}) {
        CAPTURE(knob);
        auto comp = identity(48000.0);
        comp.setTone(knob);
        comp.reset();
        std::vector<float> x(48000, 0.0f);
        x[0] = 1.0f;
        std::vector<float> y(x.size());
        for (std::size_t pos = 0; pos < x.size(); pos += 64)
            comp.process(x.data() + pos, y.data() + pos, 64);
        const auto scan = osp::rt::test::scanOutput(y);
        CHECK(scan.subnormal == 0u);
        CHECK(y.back() == 0.0f);
    }
}
