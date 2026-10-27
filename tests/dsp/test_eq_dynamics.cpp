#include "osp/dsp/Eq.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>
#include <xmmintrin.h>

using osp::dsp::Eq;
using osp::dsp::EqEdgeShape;
using osp::dsp::EqMode;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Float64 TPT bell (same equations as util/Svf.h), for the per-sample smoothing-law reference.
struct BellRef {
    double ic1 = 0.0, ic2 = 0.0;
    double tick(double fs, double hz, double q, double dB, double v0)
    {
        const double a = std::pow(10.0, dB / 40.0), g = std::tan(kPi * hz / fs), k = 1.0 / (q * a);
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return v0 + k * (a * a - 1.0) * v1;
    }
};

// Renders mono `in` in blocks of `block`, calling `change()` before the block that starts at `at`.
std::vector<float>
renderWithStep(Eq& eq, const std::vector<float>& in, int block, int at, const std::function<void()>& change)
{
    std::vector<float> out = in;
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block) {
            if (pos == at)
                change();
            float* ch[1] = {out.data() + pos};
            eq.process(ch, 1, std::min(block, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

float maxStep(const std::vector<float>& y, int from, int to)
{
    float m = 0.0f;
    for (int i = from + 1; i < to; ++i)
        m = std::max(m, std::abs(y[static_cast<std::size_t>(i)] - y[static_cast<std::size_t>(i - 1)]));
    return m;
}

} // namespace

TEST_CASE("Eq: band gain, band frequency and level steps leave <= -60 dBc residual (REQ-TEST-003)", "[dsp][eq][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const int block = GENERATE(37, 64, 512);
    const bool up = GENERATE(true, false);
    CAPTURE(fs, block, up);
    const auto tone = osp::measure::coherentTone(1000.0, fs);
    const int n = static_cast<int>(0.4 * fs);
    const auto in = osp::measure::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const int at = (n / 2 + block - 1) / block * block; // the block boundary where the change is delivered
    const double a = std::exp(-1.0 / (0.020 * fs));     // one-pole 20 ms
    auto pole = [&](int i, double from, double to) {
        return i < at ? from : to + (from - to) * std::pow(a, i - at + 1);
    };
    std::vector<double> ref(static_cast<std::size_t>(n));

    // Graphic 1 kHz band 0 <-> +12 dB (one-pole on dB, coefficients every sample).
    Eq eq;
    eq.setGraphic9Db(4, up ? 0.0f : 12.0f);
    eq.prepare(fs, 512);
    auto out = renderWithStep(eq, in, block, at, [&] { eq.setGraphic9Db(4, up ? 12.0f : 0.0f); });
    BellRef bell;
    for (int i = 0; i < n; ++i)
        ref[static_cast<std::size_t>(i)] = bell.tick(
            fs, 1000.0, Eq::kGraphic9Q, pole(i, up ? 0.0 : 12.0, up ? 12.0 : 0.0), in[static_cast<std::size_t>(i)]);
    const double rGain = osp::measure::residualDbc(out, ref);

    // Parametric p2 bell (+9 dB, Q 1) swept 500 <-> 2000 Hz across the tone (one-pole on log Hz). The block smooths
    // the float log-frequency, so the reference uses the same float endpoints.
    eq = Eq();
    eq.setMode(EqMode::parametric);
    eq.setParametric(1, up ? 500.0f : 2000.0f, 9.0f, 1.0f);
    eq.prepare(fs, 512);
    out = renderWithStep(eq, in, block, at, [&] { eq.setParametric(1, up ? 2000.0f : 500.0f, 9.0f, 1.0f); });
    bell = {};
    const double l0 = static_cast<double>(static_cast<float>(std::log(up ? 500.0 : 2000.0)));
    const double l1 = static_cast<double>(static_cast<float>(std::log(up ? 2000.0 : 500.0)));
    for (int i = 0; i < n; ++i)
        ref[static_cast<std::size_t>(i)] =
            bell.tick(fs, std::exp(pole(i, l0, l1)), 1.0, 9.0, in[static_cast<std::size_t>(i)]);
    const double rFreq = osp::measure::residualDbc(out, ref);

    // Level -12 <-> +12 dB: 20 ms linear ramp of the linear gain.
    eq = Eq();
    eq.setLevelDb(up ? -12.0f : 12.0f);
    eq.prepare(fs, 512);
    out = renderWithStep(eq, in, block, at, [&] { eq.setLevelDb(up ? 12.0f : -12.0f); });
    const double g0 = std::pow(10.0, (up ? -12.0 : 12.0) / 20.0), g1 = std::pow(10.0, (up ? 12.0 : -12.0) / 20.0);
    const int len = static_cast<int>(std::lround(0.020 * fs));
    for (int i = 0; i < n; ++i) {
        const double g = i < at ? g0 : g0 + (g1 - g0) * std::min(1.0, static_cast<double>(i - at + 1) / len);
        ref[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * g;
    }
    const double rLevel = osp::measure::residualDbc(out, ref);

    CAPTURE(rGain, rFreq, rLevel);
    REQUIRE(rGain <= -60.0);
    REQUIRE(rFreq <= -60.0);
    REQUIRE(rLevel <= -60.0);
    if (block == 64 && fs == 48000.0)
        WARN("REQ-TEST-003 eq residual dBc (" << (up ? "up" : "down") << "): gain " << rGain << ", freq " << rFreq
                                              << ", level " << rLevel);
}

TEST_CASE("Eq: mode, edge-shape and cut switches are click-free (xf 20)", "[dsp][eq][xf]")
{
    const double fs = 48000.0;
    const auto tone = osp::measure::coherentTone(997.0, fs);
    const auto in = osp::measure::makeTone(tone, fs, -6.0, 48000);
    auto configure = [](Eq& eq) {
        for (int b = 0; b < 9; ++b)
            eq.setGraphic9Db(b, b == 4 ? 12.0f : -6.0f);
        eq.setParametric(0, 2000.0f, -15.0f, 0.7f); // p1 as a shelf vs a bell at 2 kHz: very different at 1 kHz
        eq.setParametric(3, 600.0f, 15.0f, 0.7f);
        eq.setEdgeShape(0, EqEdgeShape::shelf);
    };
    const std::vector<std::function<void(Eq&)>> switches = {
        [](Eq& eq) { eq.setMode(EqMode::parametric); },
        [](Eq& eq) { eq.setEdgeShape(0, EqEdgeShape::peak); },
        // Cuts switched off (to the range end): turning one on is click-free anyway, because its frequency glides
        // in from the range end, so only the off direction exercises the on/off fade.
        [](Eq& eq) { eq.setLowCutHz(20.0f); },
        [](Eq& eq) { eq.setHighCutHz(20000.0f); },
    };
    for (std::size_t sw = 0; sw < switches.size(); ++sw)
        for (int at = 24000; at < 24048; at += 6) { // 8 phases over one period, 1-sample blocks
            CAPTURE(sw, at);
            Eq eq;
            configure(eq);
            if (sw == 1)
                eq.setMode(EqMode::parametric);
            if (sw == 2)
                eq.setLowCutHz(2000.0f);
            if (sw == 3)
                eq.setHighCutHz(2000.0f);
            eq.prepare(fs, 64);
            const auto out = renderWithStep(eq, in, 1, at, [&] { switches[sw](eq); });
            const float steady = std::max(maxStep(out, at - 2400, at), maxStep(out, at + 4800, at + 9600));
            const float after = maxStep(out, at - 1, at + 1920); // the 20 ms fade (40 ms window)
            CAPTURE(steady, after);
            REQUIRE(after <= 1.05f * steady + 2.0e-3f);
        }
}

TEST_CASE("Eq: 1000 random parameter vectors in stereo, NaN/subnormal scan, FTZ-off tail", "[dsp][eq][rt]")
{
    Eq eq;
    eq.prepare(48000.0, 512);
    std::uint32_t s = 77u;
    auto uni = [&](float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>(s >> 8) / 16777216.0f;
    };
    std::vector<float> l(512), r(512);
    std::size_t bad = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            eq.setMode(static_cast<EqMode>(static_cast<int>(uni(0.0f, 2.999f))));
            for (int b = 0; b < 9; ++b)
                eq.setGraphic9Db(b, uni(-14.0f, 14.0f));
            for (int b = 0; b < 5; ++b)
                eq.setGraphic5Db(b, uni(-14.0f, 14.0f));
            for (int b = 0; b < 4; ++b)
                eq.setParametric(b, uni(10.0f, 24000.0f), uni(-18.0f, 18.0f), uni(0.1f, 8.0f));
            eq.setEdgeShape(0, uni(0.0f, 1.0f) > 0.5f ? EqEdgeShape::shelf : EqEdgeShape::peak);
            eq.setEdgeShape(3, uni(0.0f, 1.0f) > 0.5f ? EqEdgeShape::shelf : EqEdgeShape::peak);
            eq.setLowCutHz(uni(10.0f, 600.0f));
            eq.setHighCutHz(uni(1000.0f, 24000.0f));
            eq.setLevelDb(uni(-14.0f, 14.0f));
            for (std::size_t i = 0; i < l.size(); ++i) {
                l[i] = uni(-1.0f, 1.0f);
                r[i] = uni(-1.0f, 1.0f);
            }
            float* ch[2] = {l.data(), r.data()};
            eq.process(ch, 2, v % 2 == 0 ? 512 : 127);
            const auto sl = osp::rt::test::scanOutput(l), sr = osp::rt::test::scanOutput(r);
            bad += sl.nonFinite + sl.subnormal + sr.nonFinite + sr.subnormal;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    REQUIRE(bad == 0u);

    // Impulse, then 2 s of silence with FTZ off: every state decays to exactly 0 (SVF snap), no subnormal output.
    REQUIRE((_mm_getcsr() & 0x8040u) == 0u);
    std::vector<float> tail(96000, 0.0f);
    tail[0] = 1.0f;
    for (std::size_t pos = 0; pos < tail.size(); pos += 512) {
        float* ch[1] = {tail.data() + pos};
        eq.process(ch, 1, static_cast<int>(std::min<std::size_t>(512, tail.size() - pos)));
    }
    CHECK(osp::rt::test::scanOutput(tail).subnormal == 0u);
    CHECK(tail.back() == 0.0f);
}
