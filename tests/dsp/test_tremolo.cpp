// Harmonic tremolo (#70 part 3): complementary low/high split at 600 Hz, bands modulated in antiphase.

#include "osp/dsp/Tremolo.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Tremolo;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

void render(Tremolo& t, std::vector<float>& l, std::vector<float>& r, int block, int from = 0, int to = -1)
{
    const int total = to < 0 ? static_cast<int>(l.size()) : to;
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = from; pos < total; pos += step) {
            if (block == 0)
                t.process(l.data() + pos, r.data() + pos, 0);
            t.process(l.data() + pos, r.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

Tremolo make(double fs, float depth, float mix, float width = 100.0f, float rate = 0.8f)
{
    Tremolo t;
    t.setDepthPct(depth);
    t.setMixPct(mix);
    t.setWidthPct(width);
    t.setRateHz(rate);
    t.prepare(fs, kMaxBlock);
    return t;
}

} // namespace

TEST_CASE("Tremolo: DC follows the low-band law, Nyquist the high-band law, at the exact LFO rate and depth",
          "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    for (float depth : {50.0f, 100.0f}) {
        for (float rate : {0.5f, 4.0f}) {
            // DC passes the low band only, and the alternating ±x at fs/2 the high band only (the TPT low-pass has
            // its bilinear zero there), so after the crossover settles out = x · gain law at sample n, LFO phase
            // n · rate / fs. Width 100 %: the right side runs half a cycle later.
            const int n = static_cast<int>(1.2 * fs);
            std::vector<float> dcL(static_cast<std::size_t>(n), 0.5f), dcR = dcL, nyL(dcL.size()), nyR;
            for (std::size_t i = 0; i < nyL.size(); ++i)
                nyL[i] = i % 2 == 0 ? 0.5f : -0.5f;
            nyR = nyL;
            auto a = make(fs, depth, 100.0f, 100.0f, rate), b = a;
            render(a, dcL, dcR, 127);
            render(b, nyL, nyR, 127);
            double worst = 0.0;
            for (int i = static_cast<int>(0.05 * fs); i < n; ++i) {
                const auto k = static_cast<std::size_t>(i);
                const double ph = 2.0 * std::numbers::pi * rate * i / fs;
                const auto lfoL = static_cast<float>(std::sin(ph)),
                           lfoR = static_cast<float>(std::sin(ph + std::numbers::pi));
                const float d = depth / 100.0f, sign = i % 2 == 0 ? 1.0f : -1.0f; // the input's sign
                worst = std::max({worst, std::abs(dcL[k] - 0.5 * Tremolo::lowGain(lfoL, d)),
                                  std::abs(dcR[k] - 0.5 * Tremolo::lowGain(lfoR, d)),
                                  std::abs(sign * nyL[k] - 0.5 * Tremolo::highGain(lfoL, d)),
                                  std::abs(sign * nyR[k] - 0.5 * Tremolo::highGain(lfoR, d))});
            }
            CAPTURE(fs, depth, rate);
            CHECK(worst <= 1e-4);
        }
    }
}

TEST_CASE("Tremolo: rate x block matrix; depth 0 is transparent; dry paths exact; mix ramp residual", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 127, 2048);
    CAPTURE(fs, block);
    const int n = static_cast<int>(0.3 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(3300.0, fs, n, 0.3);

    auto t0 = make(fs, 0.0f, 100.0f);
    REQUIRE(t0.getLatencySamples() == 0);
    auto l = a, r = b;
    render(t0, l, r, block);
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(std::abs(l[i] - a[i]) <= 1e-6f); // complementary bands: low + (x − low)
        REQUIRE(std::abs(r[i] - b[i]) <= 1e-6f);
    }

    for (int mode = 0; mode < 3; ++mode) {
        Tremolo t;
        t.setDepthPct(100.0f);
        if (mode == 0)
            t.setMixPct(0.0f);
        else if (mode == 1)
            t.setEnabled(false);
        else
            t.setBypassed(true);
        t.prepare(fs, kMaxBlock);
        auto dl = a, dr = b;
        render(t, dl, dr, block);
        CAPTURE(mode);
        CHECK(dl == a);
        CHECK(dr == b);
    }

    // Mix 50 -> 100 % on a non-block-aligned sample: dry + 20 ms linear law x (full wet) (REQ-TEST-003).
    auto full = make(fs, 80.0f, 100.0f), t = make(fs, 80.0f, 50.0f);
    auto wl = a, wr = b;
    render(full, wl, wr, block);
    l = a;
    r = b;
    const int step = static_cast<int>(0.1 * fs) + 37;
    render(t, l, r, block, 0, step);
    t.setMixPct(100.0f);
    render(t, l, r, block, step);
    osp::dsp::LinearRamp law{0.5f};
    law.prepare(fs, 0.020);
    std::vector<double> ref(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i == static_cast<std::size_t>(step))
            law.setTarget(1.0f);
        ref[i] = a[i] + law.next() * (static_cast<double>(wl[i]) - a[i]);
    }
    CHECK(osp::measure::residualDbc(l, ref) <= -60.0);
}

TEST_CASE("Tremolo: reset(), before prepare(), NaN input, 1000 random parameter vectors", "[dsp][mod]")
{
    Tremolo cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));

    auto t = make(48000.0, 90.0f, 100.0f, 60.0f, 5.0f);
    const auto s = sine(150.0, 48000.0, 9600, 0.5);
    auto l1 = s, r1 = s, l2 = s, r2 = s;
    render(t, l1, r1, 64);
    t.reset();
    render(t, l2, r2, 127);
    CHECK(l1 == l2);
    CHECK(r1 == r2);

    std::mt19937 rng(7003);
    std::uniform_real_distribution<float> pct(-120.0f, 120.0f), hz(-1.0f, 15.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            t.setRateHz(k % 71 == 0 ? std::nanf("") : hz(rng));
            t.setDepthPct(k % 67 == 0 ? std::nanf("") : pct(rng));
            t.setMixPct(pct(rng));
            t.setWidthPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            t.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            t.process(bl.data(), br.data(), n);
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
