// Phaser and vibe (#70 part 2): first-order TPT allpass cascades swept by the shared LFO, zero latency.

#include "osp/dsp/Phaser.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Phaser;
using Voicing = osp::dsp::Phaser::Voicing;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

void render(Phaser& p, std::vector<float>& l, std::vector<float>& r, int block, int from = 0, int to = -1)
{
    const int total = to < 0 ? static_cast<int>(l.size()) : to;
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = from; pos < total; pos += step) {
            if (block == 0)
                p.process(l.data() + pos, r.data() + pos, 0);
            p.process(l.data() + pos, r.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

// Steady-state gain in dB of a sine through the left channel (amplitude of the last quarter re the input's).
double sineGainDb(Phaser p, double fs, double hz)
{
    const int n = static_cast<int>(0.4 * fs);
    const auto in = sine(hz, fs, n, 0.25);
    auto l = in, r = in;
    render(p, l, r, 128);
    double so = 0.0, si = 0.0;
    for (std::size_t i = 3 * l.size() / 4; i < l.size(); ++i) {
        so += static_cast<double>(l[i]) * l[i];
        si += static_cast<double>(in[i]) * in[i];
    }
    return 10.0 * std::log10(so / si);
}

// Digital phase of one TPT allpass stage at hz: −2·atan(tan(π f / fs) / g), g = tan(π fc / fs) (header).
double stagePhase(double hz, double fc, double fs)
{
    return -2.0 * std::atan(std::tan(std::numbers::pi * hz / fs) / std::tan(std::numbers::pi * fc / fs));
}

Phaser make(Voicing v, double fs, float depth, float mix = 50.0f)
{
    Phaser p;
    p.setVoicing(v);
    p.setDepthPct(depth);
    p.setMixPct(mix);
    p.prepare(fs, kMaxBlock);
    return p;
}

} // namespace

TEST_CASE("Phaser: static response matches the allpass cascade; notch positions", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    CAPTURE(fs);
    const double fc = Phaser::phaserCentreHz();
    // Mix 50 %: |(1 + e^{jΦ}) / 2| = |cos(Φ / 2)|, Φ = the total cascade phase.
    for (double hz : {100.0, 300.0, 1000.0, 2500.0, 6000.0}) {
        const double phaser = 6.0 * stagePhase(hz, fc, fs);
        double vibe = 0.0;
        for (double ratio : Phaser::kVibeRatios)
            vibe += stagePhase(hz, Phaser::kVibeLowHz * ratio, fs);
        CAPTURE(hz);
        CHECK(std::abs(sineGainDb(make(Voicing::phaser, fs, 0.0f), fs, hz) -
                       20.0 * std::log10(std::abs(std::cos(phaser / 2.0)))) <= 0.05);
        CHECK(std::abs(sineGainDb(make(Voicing::vibe, fs, 0.0f), fs, hz) -
                       20.0 * std::log10(std::abs(std::cos(vibe / 2.0)))) <= 0.05);
    }
    // Six equal stages: Φ = −(2k + 1)π where atan(tan(πf/fs)/g) = (2k + 1)π/12.
    const double g = std::tan(std::numbers::pi * fc / fs);
    for (int k = 0; k < 3; ++k) {
        const double notch = fs / std::numbers::pi * std::atan(g * std::tan((2 * k + 1) * std::numbers::pi / 12.0));
        const double db = sineGainDb(make(Voicing::phaser, fs, 0.0f), fs, notch);
        WARN("phaser at " << fs << " Hz: notch " << k << " at " << notch << " Hz measures " << db << " dB");
        CHECK(db <= -40.0);
    }
}

TEST_CASE("Phaser: feedback deepens the response between notches", "[dsp][mod]")
{
    const double fs = 48000.0, fc = Phaser::phaserCentreHz();
    // Between notches 0 and 1 the cascade phase is −2π (wet in phase with dry): the feedback peak.
    const double peak =
        fs / std::numbers::pi * std::atan(std::tan(std::numbers::pi * fc / fs) * std::tan(std::numbers::pi / 6.0));
    Phaser p = make(Voicing::phaser, fs, 0.0f, 100.0f);
    p.setFeedbackPct(90.0f);
    p.reset();
    const double db = sineGainDb(p, fs, peak);
    // Wet only, unit-delay feedback: |1 / (1 − 0.9·e^{−jω})| at the peak (the cascade itself is 1 there).
    const double w = 2.0 * std::numbers::pi * peak / fs;
    const double expect = -10.0 * std::log10(1.0 - 1.8 * std::cos(w) + 0.81);
    WARN("phaser +90 % feedback at " << peak << " Hz: " << db << " dB (expected " << expect << ")");
    CHECK(std::abs(db - expect) <= 0.5);
}

TEST_CASE("Phaser: the sweep runs at the LFO rate (phaser and vibe)", "[dsp][mod]")
{
    const double fs = 48000.0;
    for (auto v : {Voicing::phaser, Voicing::vibe}) {
        for (float rate : {0.5f, 3.0f}) {
            Phaser p = make(v, fs, 100.0f, 100.0f);
            p.setRateHz(rate);
            p.reset();
            const int n = static_cast<int>((4.0 / rate + 0.2) * fs);
            auto l = sine(700.0, fs, n, 0.5), r = l;
            render(p, l, r, 256);
            // Instantaneous frequency of the wet signal: the swept phase makes it deviate once up, once down per
            // LFO period. Count upward crossings of the deviation with hysteresis.
            std::vector<double> t, dev;
            double last = -1.0;
            for (int i = static_cast<int>(0.05 * fs); i < n; ++i) {
                const float a = l[static_cast<std::size_t>(i) - 1], b = l[static_cast<std::size_t>(i)];
                if (a < 0.0f && b >= 0.0f) {
                    const double tc = i - 1 + a / (a - b);
                    if (last >= 0.0) {
                        t.push_back(0.5 * (tc + last) / fs);
                        dev.push_back(fs / (tc - last) - 700.0);
                    }
                    last = tc;
                }
            }
            const double peak = *std::max_element(dev.begin(), dev.end());
            const double trough = *std::min_element(dev.begin(), dev.end());
            std::vector<double> up;
            bool armed = false;
            for (std::size_t k = 0; k < dev.size(); ++k) {
                armed = armed || dev[k] < 0.3 * trough;
                if (armed && dev[k] >= 0.3 * peak) {
                    up.push_back(t[k]);
                    armed = false;
                }
            }
            REQUIRE(up.size() >= 3);
            const double measured = static_cast<double>(up.size() - 1) / (up.back() - up.front());
            CAPTURE(static_cast<int>(v), rate);
            WARN((v == Voicing::phaser ? "phaser" : "vibe")
                 << " at " << rate << " Hz: LFO rate " << measured << " Hz, peak deviation " << peak << " Hz");
            CHECK(std::abs(measured - rate) <= 0.02 * rate);
        }
    }
}

TEST_CASE("Phaser: rate x block matrix; mix 0, disabled, bypass exact dry; mix ramp residual", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 127, 2048);
    CAPTURE(fs, block);
    const int n = static_cast<int>(0.3 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(330.0, fs, n, 0.3);
    for (auto v : {Voicing::phaser, Voicing::vibe}) {
        for (int mode = 0; mode < 3; ++mode) {
            Phaser p;
            p.setVoicing(v);
            p.setFeedbackPct(60.0f);
            if (mode == 0)
                p.setMixPct(0.0f);
            else if (mode == 1)
                p.setEnabled(false);
            else
                p.setBypassed(true);
            p.prepare(fs, kMaxBlock);
            REQUIRE(p.getLatencySamples() == 0);
            auto l = a, r = b;
            render(p, l, r, block);
            CAPTURE(static_cast<int>(v), mode);
            CHECK(l == a);
            CHECK(r == b);
        }
        // Mix 50 -> 100 % on a non-block-aligned sample: dry + 20 ms linear law x (full wet) (REQ-TEST-003).
        Phaser full = make(v, fs, 50.0f, 100.0f), p = make(v, fs, 50.0f, 50.0f);
        auto wl = a, wr = b, l = a, r = b;
        render(full, wl, wr, block);
        const int step = static_cast<int>(0.1 * fs) + 37;
        render(p, l, r, block, 0, step);
        p.setMixPct(100.0f);
        render(p, l, r, block, step);
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
}

TEST_CASE("Phaser: reset(), before prepare(), NaN input, 1000 random parameter vectors", "[dsp][mod]")
{
    Phaser cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));

    for (auto v : {Voicing::phaser, Voicing::vibe}) {
        Phaser p = make(v, 48000.0, 80.0f);
        p.setFeedbackPct(-70.0f);
        p.reset(); // settle the feedback ramp, so both renders start from the same state
        const auto s = sine(150.0, 48000.0, 9600, 0.5);
        auto l1 = s, r1 = s, l2 = s, r2 = s;
        render(p, l1, r1, 64);
        p.reset();
        render(p, l2, r2, 127);
        CHECK(l1 == l2);
        CHECK(r1 == r2);
    }

    Phaser p = make(Voicing::phaser, 48000.0, 50.0f);
    std::mt19937 rng(7070);
    std::uniform_real_distribution<float> pct(-120.0f, 120.0f), hz(-1.0f, 15.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            p.setVoicing(coin(rng) == 1 ? Voicing::vibe : Voicing::phaser);
            p.setRateHz(k % 71 == 0 ? std::nanf("") : hz(rng));
            p.setDepthPct(k % 67 == 0 ? std::nanf("") : pct(rng));
            p.setMixPct(pct(rng));
            p.setFeedbackPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            p.setWidthPct(pct(rng));
            p.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            p.process(bl.data(), br.data(), n);
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
