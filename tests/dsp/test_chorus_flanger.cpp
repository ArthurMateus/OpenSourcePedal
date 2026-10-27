// Chorus ×2 and flanger (#70 part 1): modulated fractional delays, zero latency, LFO from util/Lfo.h.

#include "osp/dsp/ChorusFlanger.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::ChorusFlanger;
using Voicing = osp::dsp::ChorusFlanger::Voicing;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

void render(ChorusFlanger& c, std::vector<float>& l, std::vector<float>& r, int block)
{
    const int total = static_cast<int>(l.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (block == 0)
                c.process(l.data() + pos, r.data() + pos, 0);
            c.process(l.data() + pos, r.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

double rms(const std::vector<float>& x, std::size_t from)
{
    double s = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(x.size() - from));
}

// Steady-state gain in dB of a sine through the left channel.
double sineGainDb(ChorusFlanger& c, double fs, double hz)
{
    const int n = static_cast<int>(0.5 * fs);
    const auto in = sine(hz, fs, n, 0.25);
    auto l = in, r = in;
    render(c, l, r, 128);
    return 20.0 * std::log10(rms(l, l.size() / 2) / rms(in, in.size() / 2));
}

} // namespace

TEST_CASE("Lfo: the phase advances exactly rate / fs per sample at every rate", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    for (float hz : {0.1f, 0.8f, 10.0f}) {
        osp::dsp::Lfo lfo;
        lfo.setRateHz(hz);
        lfo.prepare(fs);
        lfo.reset(0.0);
        double cycles = 0.0, last = 0.0;
        for (int i = 0; i <= static_cast<int>(fs); ++i) { // one second
            const double p = lfo.next();
            cycles += p < last ? 1.0 - last + p : p - last;
            last = p;
        }
        CAPTURE(fs, hz);
        CHECK(std::abs(cycles - hz) < 1e-9 * fs);
    }
}

TEST_CASE("ChorusFlanger: rate x block matrix; zero latency; the static wet impulse sits at the base delay",
          "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    ChorusFlanger c;
    c.setDepthPct(0.0f); // static delay: both chorusWide voices at 12 ms
    c.setMixPct(100.0f);
    c.prepare(fs, kMaxBlock);
    REQUIRE(c.getLatencySamples() == 0);
    std::vector<float> l(static_cast<std::size_t>(0.03 * fs), 0.0f), r(l.size(), 0.0f);
    l[0] = 1.0f;
    render(c, l, r, block);
    CHECK(l[0] == 0.0f); // mix 100 %: nothing undelayed
    const auto expect = static_cast<std::size_t>(std::lround(0.012 * fs));
    const auto peak = static_cast<std::size_t>(
        std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - l.begin());
    CHECK((peak + 1 >= expect && peak <= expect + 1));
    double sum = 0.0;
    for (float v : l)
        sum += v;
    CHECK(std::abs(sum - 1.0) < 1e-3); // two voices at 0.5 each: unity DC gain
    for (float v : r)
        CHECK(v == 0.0f);
}

TEST_CASE("ChorusFlanger: pitch deviation and LFO rate measured on the wet voice", "[dsp][mod]")
{
    const double fs = 48000.0;
    for (float rate : {0.5f, 2.0f, 6.0f}) {
        ChorusFlanger c;
        c.setVoicing(Voicing::chorus80s);
        c.setMixPct(100.0f);
        c.setDepthPct(100.0f);
        c.setRateHz(rate);
        c.prepare(fs, kMaxBlock);
        const int n = static_cast<int>((4.0 / rate + 0.2) * fs); // 4 LFO periods after the start-up
        auto l = sine(1000.0, fs, n, 0.5), r = l;
        render(c, l, r, 256);
        // Instantaneous frequency (cents re 1 kHz) from upward zero crossings of the wet signal.
        std::vector<double> t, cents;
        double last = -1.0;
        for (int i = static_cast<int>(0.05 * fs); i < n; ++i) {
            const float a = l[static_cast<std::size_t>(i) - 1], b = l[static_cast<std::size_t>(i)];
            if (a < 0.0f && b >= 0.0f) {
                const double tc = i - 1 + a / (a - b);
                if (last >= 0.0) {
                    t.push_back(0.5 * (tc + last) / fs);
                    cents.push_back(1200.0 * std::log2(fs / (tc - last) / 1000.0));
                }
                last = tc;
            }
        }
        // A sine sweep of ±M s at f Hz gives a pitch factor 1 − dτ/dt, peaking at 1 + 2πfM (header).
        const double expected = -1200.0 * std::log2(1.0 - 2.0 * std::numbers::pi * rate * 0.003);
        double worst = 0.0;
        std::vector<double> up; // upward crossings of the deviation, with hysteresis: one per LFO period
        bool armed = false;
        for (std::size_t k = 0; k < cents.size(); ++k) {
            worst = std::max(worst, std::abs(cents[k]));
            armed = armed || cents[k] < -0.5 * expected;
            if (armed && cents[k] >= 0.0) {
                up.push_back(t[k]);
                armed = false;
            }
        }
        REQUIRE(up.size() >= 3);
        const double measuredRate = static_cast<double>(up.size() - 1) / (up.back() - up.front());
        CAPTURE(rate);
        WARN("chorus80s at " << rate << " Hz: peak deviation " << worst << " cents (expected " << expected
                             << "), LFO rate " << measuredRate << " Hz");
        CHECK(std::abs(worst - expected) <= 0.05 * expected + 1.0);
        CHECK(std::abs(measuredRate - rate) <= 0.01 * rate);
    }
}

TEST_CASE("ChorusFlanger: flanger comb notch and peak, feedback raises the peak", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    CAPTURE(fs);
    // Depth 0: τ = 0.25 ms. Mix 50 %: out = (x + x(t − τ)) / 2, notches at (2k + 1) / (2τ), peaks at k / τ.
    auto make = [&](float fb) {
        ChorusFlanger c;
        c.setVoicing(Voicing::flanger);
        c.setDepthPct(0.0f);
        c.setFeedbackPct(fb);
        c.prepare(fs, kMaxBlock);
        return c;
    };
    auto c0 = make(0.0f), c1 = make(0.0f), c2 = make(90.0f);
    const double notch = sineGainDb(c0, fs, 2000.0), peak = sineGainDb(c1, fs, 4000.0);
    const double fbPeak = sineGainDb(c2, fs, 4000.0);
    WARN("flanger at " << fs << " Hz: notch 2 kHz " << notch << " dB, peak 4 kHz " << peak << " dB, peak with +90 % "
                       << fbPeak << " dB");
    CHECK(notch <= -30.0);
    CHECK(std::abs(peak) <= 0.1);
    // Comb with feedback g: w = x / (1 − g) at the peak, out = (x + w) / 2 = 5.5 x for g = 0.9.
    CHECK(std::abs(fbPeak - 20.0 * std::log10(5.5)) <= 0.5);
}

TEST_CASE("ChorusFlanger: mix 0, disabled and bypass are the exact dry; mix ramp residual", "[dsp][mod]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(0.3 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(330.0, fs, n, 0.3);

    SECTION("mix 0, disabled and bypassed, every voicing")
    {
        for (auto v : {Voicing::chorus80s, Voicing::chorusWide, Voicing::flanger}) {
            for (int mode = 0; mode < 3; ++mode) {
                ChorusFlanger c;
                c.setVoicing(v);
                c.setFeedbackPct(50.0f);
                if (mode == 0)
                    c.setMixPct(0.0f);
                else if (mode == 1)
                    c.setEnabled(false);
                else
                    c.setBypassed(true);
                c.prepare(fs, kMaxBlock);
                auto l = a, r = b;
                render(c, l, r, 127);
                CAPTURE(static_cast<int>(v), mode);
                CHECK(l == a);
                CHECK(r == b);
            }
        }
    }
    SECTION("mix 50 -> 100 % follows its 20 ms linear ramp (REQ-TEST-003 <= -60 dBc)")
    {
        ChorusFlanger full;
        full.setMixPct(100.0f);
        full.prepare(fs, kMaxBlock);
        auto wl = a, wr = b;
        render(full, wl, wr, 64);
        ChorusFlanger c;
        c.prepare(fs, kMaxBlock); // mix 50 %
        c.setMixPct(100.0f);
        auto l = a, r = b;
        render(c, l, r, 64);
        osp::dsp::LinearRamp law{0.5f};
        law.prepare(fs, 0.020);
        law.setTarget(1.0f);
        std::vector<double> ref(a.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            ref[i] = a[i] + law.next() * (static_cast<double>(wl[i]) - a[i]);
        const double res = osp::measure::residualDbc(l, ref);
        WARN("mix ramp residual " << res << " dBc");
        CHECK(res <= -60.0);
    }
}

TEST_CASE("ChorusFlanger: reset(), before prepare(), NaN input, 1000 random parameter vectors", "[dsp][mod]")
{
    ChorusFlanger cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));

    ChorusFlanger c;
    c.setVoicing(Voicing::flanger);
    c.setFeedbackPct(-80.0f);
    c.prepare(48000.0, kMaxBlock);
    const auto s = sine(150.0, 48000.0, 9600, 0.5);
    auto l1 = s, r1 = s, l2 = s, r2 = s;
    render(c, l1, r1, 64);
    c.reset();
    render(c, l2, r2, 127);
    CHECK(l1 == l2);
    CHECK(r1 == r2);

    std::mt19937 rng(70);
    std::uniform_real_distribution<float> pct(-120.0f, 120.0f), hz(-1.0f, 15.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1), voicing(0, 2);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            c.setVoicing(static_cast<Voicing>(voicing(rng)));
            c.setRateHz(k % 71 == 0 ? std::nanf("") : hz(rng));
            c.setDepthPct(pct(rng));
            c.setMixPct(k % 59 == 0 ? std::nanf("") : pct(rng));
            c.setFeedbackPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            c.setWidthPct(pct(rng));
            c.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            c.process(bl.data(), br.data(), n);
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
