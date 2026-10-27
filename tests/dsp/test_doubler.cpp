// Doubler (#74, REQ-418): two modulated short delays, panned, added to the undelayed dry signal.

#include "osp/dsp/Doubler.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Doubler;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

void render(Doubler& d, std::vector<float>& l, std::vector<float>& r, int block)
{
    const int total = static_cast<int>(l.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (block == 0)
                d.process(l.data() + pos, r.data() + pos, 0);
            d.process(l.data() + pos, r.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

} // namespace

TEST_CASE("Doubler: rate x block matrix; zero latency; the wet impulse sits at the offset", "[dsp][doubler]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    Doubler d;
    d.setDetuneCents(0.0f); // a fixed delay, so the impulse lands on one spot
    d.setAmountPct(100.0f);
    d.prepare(fs, kMaxBlock);
    REQUIRE(d.getLatencySamples() == 0);
    std::vector<float> l(static_cast<std::size_t>(0.05 * fs), 0.0f), r(l.size(), 0.0f);
    l[0] = 1.0f;
    render(d, l, r, block);
    CHECK(l[0] == 1.0f); // the dry path is not delayed (L_dbl = 0)
    const auto expect = static_cast<std::size_t>(std::lround(0.018 * fs));
    const auto peak = static_cast<std::size_t>(
        std::max_element(l.begin() + 1, l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) -
        l.begin());
    CHECK((peak >= expect - 1 && peak <= expect + 1));
    // Width 100 %: voice A fully left at -3 dB. The offset can fall between samples (18 ms = 793.8 samples at
    // 44.1 kHz), where the interpolation spreads the impulse over its neighbours, so check its sum (the DC gain).
    double wet = 0.0;
    for (std::size_t i = 1; i < l.size(); ++i)
        wet += l[i];
    CHECK(std::abs(wet - Doubler::kWetGain) < 1e-3);
    for (float v : r)
        CHECK(v == 0.0f); // nothing on the right input, so nothing of A on the right
}

TEST_CASE("Doubler: the voices' pitch deviation follows detuneCents", "[dsp][doubler]")
{
    const double fs = 48000.0;
    for (float cents : {8.0f, 25.0f}) {
        Doubler d;
        d.setAmountPct(100.0f);
        d.setDetuneCents(cents);
        d.prepare(fs, kMaxBlock);
        const int n = static_cast<int>(4.0 * fs); // > 2 LFO periods of voice A
        const auto in = sine(1000.0, fs, n, 0.5);
        auto l = in, r = std::vector<float>(in.size(), 0.0f);
        render(d, l, r, 256);
        // The wet voice alone (left minus dry), its frequency from upward zero crossings.
        double worst = 0.0;
        double last = -1.0;
        for (int i = static_cast<int>(0.1 * fs); i < n; ++i) {
            const float a = l[static_cast<std::size_t>(i) - 1] - in[static_cast<std::size_t>(i) - 1];
            const float b = l[static_cast<std::size_t>(i)] - in[static_cast<std::size_t>(i)];
            if (a < 0.0f && b >= 0.0f) {
                const double t = i - 1 + a / (a - b);
                if (last >= 0.0)
                    worst = std::max(worst, std::abs(1200.0 * std::log2(fs / (t - last) / 1000.0)));
                last = t;
            }
        }
        CAPTURE(cents);
        WARN("detune " << cents << " cents: measured peak deviation " << worst << " cents");
        CHECK(std::abs(worst - cents) <= 0.15 * cents + 0.5);
    }
}

TEST_CASE("Doubler: width, amount 0, disabled, bypass, amount ramp residual", "[dsp][doubler]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(0.3 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(330.0, fs, n, 0.3);

    SECTION("width 0: both outputs carry both voices equally")
    {
        Doubler d;
        d.setWidthPct(0.0f);
        d.prepare(fs, kMaxBlock);
        auto l = a, r = a; // identical inputs: identical outputs at width 0
        render(d, l, r, 64);
        CHECK(l == r);
    }
    SECTION("amount 0, disabled and bypassed are the exact dry signal")
    {
        for (int mode = 0; mode < 3; ++mode) {
            Doubler d;
            if (mode == 0)
                d.setAmountPct(0.0f);
            else if (mode == 1)
                d.setEnabled(false);
            else
                d.setBypassed(true);
            d.prepare(fs, kMaxBlock);
            auto l = a, r = b;
            render(d, l, r, 127);
            CAPTURE(mode);
            CHECK(l == a);
            CHECK(r == b);
        }
    }
    SECTION("amount 50 -> 100 % follows its 20 ms linear ramp (REQ-TEST-003 <= -60 dBc)")
    {
        Doubler full;
        full.setAmountPct(100.0f);
        full.prepare(fs, kMaxBlock);
        auto wl = a, wr = b;
        render(full, wl, wr, 64);
        Doubler d;
        d.prepare(fs, kMaxBlock); // amount 50 %
        d.setAmountPct(100.0f);
        auto l = a, r = b;
        render(d, l, r, 64);
        osp::dsp::LinearRamp law{0.5f};
        law.prepare(fs, 0.020);
        law.setTarget(1.0f);
        std::vector<double> ref(a.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            ref[i] = a[i] + law.next() * (static_cast<double>(wl[i]) - a[i]); // dry + ramp x (full wet)
        CHECK(osp::measure::residualDbc(l, ref) <= -60.0);
    }
}

TEST_CASE("Doubler: reset(), before prepare(), NaN input, 1000 random parameter vectors", "[dsp][doubler]")
{
    Doubler cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));

    Doubler d;
    d.prepare(48000.0, kMaxBlock);
    const auto s = sine(150.0, 48000.0, 9600, 0.5);
    auto l1 = s, r1 = s, l2 = s, r2 = s;
    render(d, l1, r1, 64);
    d.reset();
    render(d, l2, r2, 127);
    CHECK(l1 == l2);
    CHECK(r1 == r2);

    std::mt19937 rng(744);
    std::uniform_real_distribution<float> pct(-20.0f, 120.0f), ms(0.0f, 40.0f), ct(-5.0f, 40.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            d.setAmountPct(k % 71 == 0 ? std::nanf("") : pct(rng));
            d.setWidthPct(pct(rng));
            d.setDelayMs(k % 59 == 0 ? std::nanf("") : ms(rng));
            d.setDetuneCents(ct(rng));
            d.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = sig(rng);
            }
            d.process(bl.data(), br.data(), n);
            for (int i = 0; i < n; ++i)
                nonFinite +=
                    std::isfinite(bl[static_cast<std::size_t>(i)]) && std::isfinite(br[static_cast<std::size_t>(i)])
                        ? 0
                        : 1;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0);
}
