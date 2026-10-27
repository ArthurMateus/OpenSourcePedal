// Delay voice, digital (#71 part 2): echo time, feedback decay and the 0.995 loop cap, ping-pong, dry paths, mix ramp
// residual, rate × block matrix under the RT checker, reset, pre-prepare and a 1000-vector NaN fuzz.

#include "osp/dsp/delay/DelayVoice.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::DelayVoice;
using Stereo = osp::dsp::DelayVoice::Stereo;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

// Wet only (wet = true) or in place, in calls of `block` samples (0: a zero-length call before each 64-sample call).
void render(DelayVoice& v, std::vector<float>& l, std::vector<float>& r, int block, bool wet)
{
    const int total = static_cast<int>(l.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            float *pl = l.data() + pos, *pr = r.data() + pos;
            if (block == 0)
                v.process(pl, pr, 0);
            if (wet)
                v.processWet(pl, pr, pl, pr, n);
            else
                v.process(pl, pr, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

// A clean digital voice: widest cuts, so the loop is as close to a pure delay as the parameters allow.
void clean(DelayVoice& v, double fs, float ms, float fbPct, Stereo s)
{
    v.setTimeMs(ms);
    v.setFeedbackPct(fbPct);
    v.setLowCutHz(50.0f);
    v.setHighCutHz(16000.0f);
    v.setStereo(s);
    v.prepare(fs, kMaxBlock);
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t len)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + len; ++i)
        s += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(s / static_cast<double>(len) + 1e-300);
}

std::ptrdiff_t peakAt(const std::vector<float>& y, std::size_t from, std::size_t to)
{
    return std::distance(y.begin(), std::max_element(y.begin() + static_cast<std::ptrdiff_t>(from),
                                                     y.begin() + static_cast<std::ptrdiff_t>(to)));
}

} // namespace

TEST_CASE("DelayVoice: echo time within +-1 sample, ping-pong alternates one delay time apart", "[dsp][delay]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const float ms = GENERATE(7.0f, 123.4567f, 400.0f, 3000.0f);
    CAPTURE(fs, ms);
    const double d = ms * fs / 1000.0;
    const auto len = static_cast<std::size_t>(2.0 * d) + 64;
    {
        DelayVoice v;
        clean(v, fs, ms, 0.0f, Stereo::mono);
        std::vector<float> l(len, 0.0f), r(len, 0.0f);
        l[0] = r[0] = 1.0f;
        render(v, l, r, 127, true);
        CHECK(std::abs(static_cast<double>(peakAt(l, 0, len)) - d) <= 1.0);
        CHECK(l == r);
    }
    // With the cuts in the loop, the echo is smeared, so its time is its 1 kHz group delay (ADR-0005 D4's measure).
    // The high cut's DC delay, 1/(2g), is compensated; the 50 Hz low cut adds at most 0.76 sample at 1 kHz (96 kHz).
    for (float highCut : {5000.0f, 16000.0f}) {
        DelayVoice v;
        clean(v, fs, ms, 0.0f, Stereo::mono);
        v.setHighCutHz(highCut);
        v.reset();
        const auto tail = static_cast<std::size_t>(d + 0.2 * fs); // the 50 Hz low cut's tail has decayed by then
        std::vector<float> l(tail, 0.0f), r(tail, 0.0f);
        l[0] = r[0] = 1.0f;
        render(v, l, r, 64, true);
        const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
        std::complex<double> num, den;
        for (std::size_t i = 0; i < tail; ++i) {
            const auto e = std::polar(static_cast<double>(l[i]), -w * static_cast<double>(i));
            num += static_cast<double>(i) * e;
            den += e;
        }
        CAPTURE(highCut, (num / den).real());
        CHECK(std::abs((num / den).real() - d) <= 1.0);
    }
    {
        DelayVoice v;
        clean(v, fs, ms, 50.0f, Stereo::pingpong);
        std::vector<float> l(len, 0.0f), r(len, 0.0f);
        l[0] = 1.0f; // (L + R) / 2 goes in: 0.5
        render(v, l, r, 127, true);
        const auto half = static_cast<std::size_t>(1.5 * d);
        CHECK(std::abs(static_cast<double>(peakAt(l, 0, half)) - d) <= 1.0);
        CHECK(std::abs(static_cast<double>(peakAt(r, 0, len)) - 2.0 * d) <= 1.0);
        CHECK(*std::max_element(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(half)) == 0.0f);
    }
}

TEST_CASE("DelayVoice: feedback decay per repeat is 0.995 x feedback; 100 % rings but decays", "[dsp][delay]")
{
    const double fs = 48000.0;
    const float fb = GENERATE(30.0f, 60.0f, 100.0f);
    const auto mode = GENERATE(Stereo::mono, Stereo::pingpong, Stereo::wide);
    CAPTURE(fb, static_cast<int>(mode));
    const float ms = 200.0f;
    const auto period = static_cast<std::size_t>(ms * fs / 1000.0);
    DelayVoice v;
    clean(v, fs, ms, fb, mode);
    // A 50 ms 1 kHz burst, where both cuts are flat within 0.02 dB (|H| = 0.9988 · 0.9993).
    auto l = sine(1000.0, fs, static_cast<int>(0.050 * fs), 0.5);
    l.resize(period * 8, 0.0f);
    auto r = l;
    render(v, l, r, 64, true);
    const auto win = static_cast<std::size_t>(0.040 * fs), off = static_cast<std::size_t>(0.005 * fs);
    // Mono and wide repeat on the left every period; ping-pong every other period on each side.
    const std::size_t stride = mode == Stereo::pingpong ? 2 : 1;
    const double perRepeat = 20.0 * std::log10(0.995 * fb / 100.0 * 0.9988 * 0.9993);
    for (std::size_t k = 1; k + stride <= 7; k += stride) {
        const double a = rmsDb(l, k * period + off, win), b = rmsDb(l, (k + stride) * period + off, win);
        CAPTURE(k, a, b);
        CHECK(std::abs((b - a) - static_cast<double>(stride) * perRepeat) <= 0.05);
    }
}

TEST_CASE("DelayVoice: mix 0, disabled and bypass are the exact dry; mix ramp residual", "[dsp][delay]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(0.5 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(330.0, fs, n, 0.3);

    SECTION("mix 0, disabled and bypassed, every stereo mode, at 100 % feedback")
    {
        for (auto s : {Stereo::mono, Stereo::pingpong, Stereo::wide}) {
            for (int mode = 0; mode < 3; ++mode) {
                DelayVoice v;
                clean(v, fs, 20.0f, 100.0f, s);
                if (mode == 0)
                    v.setMixPct(0.0f);
                else if (mode == 1)
                    v.setEnabled(false);
                else
                    v.setBypassed(true);
                v.reset();
                auto l = a, r = b;
                render(v, l, r, 127, false);
                CAPTURE(static_cast<int>(s), mode);
                CHECK(l == a);
                CHECK(r == b);
            }
        }
    }
    SECTION("mix 25 -> 100 % follows its 20 ms linear ramp (REQ-TEST-003 <= -60 dBc)")
    {
        DelayVoice w;
        clean(w, fs, 50.0f, 40.0f, Stereo::wide);
        auto wl = a, wr = b;
        render(w, wl, wr, 64, true);
        DelayVoice v;
        clean(v, fs, 50.0f, 40.0f, Stereo::wide); // mix 25 %
        v.setMixPct(100.0f);
        auto l = a, r = b;
        render(v, l, r, 64, false);
        osp::dsp::LinearRamp law{0.25f};
        law.prepare(fs, 0.020);
        law.setTarget(1.0f);
        std::vector<double> ref(a.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            ref[i] = a[i] + law.next() * static_cast<double>(wl[i]);
        const double res = osp::measure::residualDbc(l, ref);
        WARN("mix ramp residual " << res << " dBc");
        CHECK(res <= -60.0);
    }
}

TEST_CASE("DelayVoice: every rate x block size (0, 1, 127, 2048) under the RT checker is bit-identical", "[dsp][delay]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 127, 2048);
    CAPTURE(fs, block);
    const auto in = sine(440.0, fs, static_cast<int>(0.4 * fs), 0.5);
    auto make = [&](DelayVoice& v) {
        v.setStereo(Stereo::pingpong);
        v.setTimeMs(30.0f);
        v.setFeedbackPct(70.0f);
        v.prepare(fs, kMaxBlock);
        v.setTimeMs(45.0f); // a glide and a cut sweep inside the render
        v.setHighCutHz(2000.0f);
    };
    DelayVoice ref, v;
    make(ref);
    make(v);
    auto rl = in, rr = in, l = in, r = in;
    render(ref, rl, rr, 64, false);
    render(v, l, r, block, false);
    CHECK(v.getLatencySamples() == 0);
    CHECK(l == rl);
    CHECK(r == rr);
}

TEST_CASE("DelayVoice: reset(), before prepare(), NaN input, 1000 random parameter vectors", "[dsp][delay]")
{
    DelayVoice cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));

    DelayVoice v;
    clean(v, 48000.0, 25.0f, 90.0f, Stereo::wide);
    const auto s = sine(150.0, 48000.0, 9600, 0.5);
    auto l1 = s, r1 = s, l2 = s, r2 = s;
    render(v, l1, r1, 64, false);
    v.reset();
    render(v, l2, r2, 127, false);
    CHECK(l1 == l2);
    CHECK(r1 == r2);

    std::mt19937 rng(71);
    std::uniform_real_distribution<float> pct(-20.0f, 120.0f), ms(-10.0f, 3500.0f), hz(0.0f, 20000.0f),
        sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1), stereo(0, 2);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            v.setStereo(static_cast<Stereo>(stereo(rng)));
            v.setTimeMs(k % 71 == 0 ? std::nanf("") : ms(rng));
            v.setFeedbackPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            v.setMixPct(k % 59 == 0 ? std::nanf("") : pct(rng));
            v.setLowCutHz(k % 53 == 0 ? INFINITY : hz(rng));
            v.setHighCutHz(k % 67 == 0 ? std::nanf("") : hz(rng));
            v.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            v.process(bl.data(), br.data(), n);
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
