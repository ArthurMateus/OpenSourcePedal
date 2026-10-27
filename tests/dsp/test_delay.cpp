// The `delay` block (#71 part 4): mode/stereo crossfade (xf 30) with the repeats carried over, tempo sync, ducking,
// soft bypass (trails), dry paths, chunked calls, rate × block matrix under the RT checker, reset, pre-prepare and a
// 1000-vector NaN fuzz.

#include "osp/dsp/Delay.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Delay;
using osp::dsp::DelayVoice;
using osp::dsp::SyncDivision;
using Mode = osp::dsp::Delay::Mode;
using Stereo = osp::dsp::Delay::Stereo;

namespace {

constexpr int kMaxBlock = 512;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

// Renders [from, to) in calls of `block` samples (0: a zero-length call before each 64-sample call).
template <typename P>
void render(
    P& p, std::vector<float>& l, std::vector<float>& r, int block, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    to = std::min(to, l.size());
    const auto step = static_cast<std::size_t>(block > 0 ? block : 64);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = from; pos < to; pos += step) {
            const int n = static_cast<int>(std::min(step, to - pos));
            if (block == 0)
                p.process(l.data() + pos, r.data() + pos, 0);
            p.process(l.data() + pos, r.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

template <typename P>
void configure(P& p, float ms)
{
    p.setTimeMs(ms);
    p.setFeedbackPct(60.0f);
    p.setMixPct(100.0f);
    p.setModDepthPct(40.0f);
}

} // namespace

TEST_CASE("Delay: mode and stereo changes crossfade over 30 ms with the repeats carried over", "[dsp][delay]")
{
    const double fs = 48000.0;
    const auto in = sine(330.0, fs, static_cast<int>(0.6 * fs), 0.4);
    const std::size_t at = 12345; // non-block-aligned
    const auto change = GENERATE(0, 1);
    CAPTURE(change);

    Delay d;
    configure(d, 40.0f);
    d.prepare(fs, kMaxBlock);
    auto l = in, r = in;
    render(d, l, r, 64, 0, at);
    if (change == 0)
        d.setMode(Mode::tape);
    else
        d.setStereo(Stereo::pingpong);
    render(d, l, r, 64, at);

    // Reference: one voice to `at`, then a copy of it switched, mixed by the same 30 ms linear ramp.
    DelayVoice oldV, newV;
    configure(oldV, 40.0f);
    oldV.prepare(fs, kMaxBlock);
    newV.prepare(fs, kMaxBlock);
    std::vector<float> ol(in.size()), orr(in.size()), nl(in.size()), nr(in.size());
    oldV.processWet(in.data(), in.data(), ol.data(), orr.data(), static_cast<int>(at));
    newV.copyStateFrom(oldV);
    if (change == 0)
        newV.setVoicing(Mode::tape);
    else
        newV.setStereo(Stereo::pingpong);
    const int rest = static_cast<int>(in.size() - at);
    oldV.processWet(in.data() + at, in.data() + at, ol.data() + at, orr.data() + at, rest);
    newV.processWet(in.data() + at, in.data() + at, nl.data() + at, nr.data() + at, rest);
    osp::dsp::LinearRamp x{0.0f};
    x.prepare(fs, Delay::kTypeFadeSeconds);
    x.setTarget(1.0f);
    double worst = 0.0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const float g = i < at ? 0.0f : x.next();
        const double refL = in[i] + ((1.0 - g) * ol[i] + g * (i < at ? 0.0f : nl[i]));
        const double refR = in[i] + ((1.0 - g) * orr[i] + g * (i < at ? 0.0f : nr[i]));
        worst = std::max({worst, std::abs(l[i] - refL), std::abs(r[i] - refR)});
    }
    WARN("crossfade max deviation " << worst);
    CHECK(worst <= 1e-6);
    CHECK_FALSE(d.isCrossfading());
    CHECK(d.mode() == (change == 0 ? Mode::tape : Mode::digital));
}

TEST_CASE("Delay: tempo sync, ducking, soft bypass (trails), dry paths, chunked calls", "[dsp][delay]")
{
    const double fs = 48000.0;
    SECTION("sync 1/4 at 120 bpm is a 500 ms delay, bit-identical; off or no tempo falls back to timeMs")
    {
        Delay d;
        configure(d, 123.0f);
        d.setSync(SyncDivision::n1_4);
        d.setTempoBpm(120.0);
        CHECK(d.effectiveTimeMs() == 500.0f);
        d.prepare(fs, kMaxBlock);
        DelayVoice v;
        configure(v, 500.0f);
        v.prepare(fs, kMaxBlock);
        const auto in = sine(200.0, fs, static_cast<int>(1.2 * fs), 0.3);
        auto l = in, r = in, wl = in, wr = in;
        render(d, l, r, 127);
        v.processWet(wl.data(), wr.data(), wl.data(), wr.data(), static_cast<int>(in.size()));
        bool same = true;
        for (std::size_t i = 0; i < in.size(); ++i)
            same = same && l[i] == in[i] + wl[i];
        CHECK(same);
        d.setTempoBpm(0.0);
        CHECK(d.effectiveTimeMs() == 123.0f);
        d.setTempoBpm(30.0);
        d.setSync(SyncDivision::n1_1d); // 12 s, clamped
        CHECK(d.effectiveTimeMs() == 3000.0f);
        d.setSync(SyncDivision::off);
        CHECK(d.effectiveTimeMs() == 123.0f);
    }
    SECTION("ducking 12 dB: the wet is 12 dB down while playing and back to 0 dB after the release")
    {
        const auto sec = static_cast<std::size_t>(fs);
        auto in = sine(200.0, fs, static_cast<int>(2 * sec + sec / 4), 0.5);
        std::fill(in.begin() + static_cast<std::ptrdiff_t>(sec), in.end(), 0.0f); // 1 s of playing, then rest
        Delay d, plain;
        for (Delay* p : {&d, &plain}) {
            configure(*p, 100.0f);
            p->prepare(fs, kMaxBlock);
        }
        d.setDuckDb(12.0f);
        auto l = in, r = in, pl = in, pr = in;
        render(d, l, r, 64);
        render(plain, pl, pr, 64);
        auto wetDb = [&](const std::vector<float>& y, std::size_t from, std::size_t len) {
            double s = 0.0;
            for (std::size_t i = from; i < from + len; ++i)
                s += (static_cast<double>(y[i]) - in[i]) * (static_cast<double>(y[i]) - in[i]);
            return 10.0 * std::log10(s / static_cast<double>(len));
        };
        const auto win = static_cast<std::size_t>(0.2 * fs);
        // The 200 ms release takes about 0.8 s from -8 dBFS down to the -42 dBFS floor.
        const double playing = wetDb(l, sec / 2, win) - wetDb(pl, sec / 2, win);
        const double resting = wetDb(l, 2 * sec, win) - wetDb(pl, 2 * sec, win);
        WARN("ducking while playing " << playing << " dB, after the release " << resting << " dB");
        CHECK(std::abs(playing + 12.0) <= 0.1);
        CHECK(std::abs(resting) <= 0.1);
    }
    SECTION("soft bypass mutes the send and lets the repeats ring out; disabled and bypassed are the exact dry")
    {
        const auto n = static_cast<std::size_t>(1.0 * fs);
        std::vector<float> in(n, 0.0f);
        for (std::size_t i = 0; i < 480; ++i)
            in[i] = in[i + n / 2] = 0.5f; // a burst at 0 and one at 0.5 s
        // Reference: the same switch with no second burst. After the switch, both wets must be the first burst's tail.
        auto inRef = in;
        std::fill(inRef.begin() + static_cast<std::ptrdiff_t>(n / 2), inRef.end(), 0.0f);
        std::vector<float> wet[2];
        for (int k = 0; k < 2; ++k) {
            const auto& x = k == 0 ? in : inRef;
            Delay d;
            configure(d, 100.0f);
            d.prepare(fs, kMaxBlock);
            auto l = x, r = x;
            render(d, l, r, 64, 0, n / 4);
            d.setSoftBypass(true);
            render(d, l, r, 64, n / 4);
            wet[k].resize(n);
            for (std::size_t i = 0; i < n; ++i)
                wet[k][i] = l[i] - x[i];
        }
        double tail = 0.0, diff = 0.0;
        for (std::size_t i = n / 2; i < n; ++i) {
            tail += static_cast<double>(wet[1][i]) * wet[1][i];
            diff = std::max(diff, static_cast<double>(std::abs(wet[0][i] - wet[1][i])));
        }
        CHECK(tail > 1e-2); // the first burst still repeats after the switch
        CHECK(diff <= 1e-6); // the second burst is not sent

        const auto a = sine(220.0, fs, 20000, 0.4), b = sine(330.0, fs, 20000, 0.3);
        for (int mode = 0; mode < 3; ++mode) {
            Delay q;
            configure(q, 20.0f);
            q.setFeedbackPct(100.0f);
            if (mode == 0)
                q.setSoftBypass(true);
            else if (mode == 1)
                q.setEnabled(false);
            q.prepare(fs, kMaxBlock);
            if (mode == 2)
                q.setBypassed(true);
            auto ql = a, qr = b;
            render(q, ql, qr, 127);
            CAPTURE(mode);
            CHECK(ql == a);
            CHECK(qr == b);
        }
    }
    SECTION("calls longer than maxBlockSize equal maxBlockSize calls; enable ramp, idle, re-enable from silence")
    {
        const auto in = sine(150.0, fs, 30000, 0.5);
        Delay a, b;
        for (Delay* p : {&a, &b}) {
            configure(*p, 30.0f);
            p->setStereo(Stereo::wide);
            p->prepare(fs, kMaxBlock);
        }
        auto al = in, ar = in, bl = in, br = in;
        render(a, al, ar, kMaxBlock);
        render(b, bl, br, 3000);
        CHECK(al == bl);
        CHECK(ar == br);
        a.setEnabled(false);
        auto l = in, r = in;
        render(a, l, r, 64);
        CHECK(std::equal(l.begin() + 1000, l.end(), in.begin() + 1000)); // settled after the 10 ms ramp
        a.setEnabled(true);
        Delay fresh;
        configure(fresh, 30.0f);
        fresh.setStereo(Stereo::wide);
        fresh.prepare(fs, kMaxBlock);
        auto l2 = in, r2 = in, fl = in, fr = in;
        render(a, l2, r2, 64);
        render(fresh, fl, fr, 64);
        double worst = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(l2[i] - fl[i])));
        CHECK(worst <= 0.3); // only the 10 ms re-enable ramp differs: no stale tail comes back
        CHECK(std::equal(l2.begin() + 1000, l2.end(), fl.begin() + 1000));
    }
}

TEST_CASE("Delay: rate x block size matrix under the RT checker, reset(), pre-prepare", "[dsp][delay]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 127, 2048);
    CAPTURE(fs, block);
    const auto in = sine(440.0, fs, static_cast<int>(0.25 * fs), 0.5);
    auto make = [&](Delay& d) {
        configure(d, 25.0f);
        d.setDuckDb(6.0f);
        d.prepare(fs, kMaxBlock);
    };
    Delay ref, d;
    make(ref);
    make(d);
    auto rl = in, rr = in, l = in, r = in;
    // A mode change at sample 1000 (fixed by splitting the render), then a stereo change at 6000.
    for (auto* p : {&ref, &d}) {
        auto& pl = p == &ref ? rl : l;
        auto& pr = p == &ref ? rr : r;
        const int blk = p == &ref ? 64 : block;
        render(*p, pl, pr, blk, 0, 1000);
        p->setMode(Mode::analog);
        render(*p, pl, pr, blk, 1000, 6000);
        p->setStereo(Stereo::pingpong);
        render(*p, pl, pr, blk, 6000);
    }
    CHECK(d.getLatencySamples() == 0);
    CHECK(l == rl);
    CHECK(r == rr);
    d.reset();
    ref.reset();
    auto l2 = in, r2 = in, rl2 = in, rr2 = in;
    render(d, l2, r2, 127);
    render(ref, rl2, rr2, 64);
    CHECK(l2 == rl2);

    Delay cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.process(cl.data(), cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));
}

TEST_CASE("Delay: 1000 random parameter vectors incl. NaN/Inf, tempo and sync, under the RT checker", "[dsp][delay]")
{
    Delay d;
    d.prepare(48000.0, kMaxBlock);
    std::mt19937 rng(714);
    std::uniform_real_distribution<float> pct(-20.0f, 120.0f), ms(-10.0f, 3500.0f), hz(0.0f, 20000.0f),
        db(-5.0f, 30.0f), sig(-1.0f, 1.0f);
    std::uniform_real_distribution<double> bpm(-10.0, 400.0);
    std::uniform_int_distribution<int> len(0, 3 * kMaxBlock), coin(0, 1), three(0, 2), div(-1, 23);
    std::vector<float> bl(3 * kMaxBlock), br(3 * kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            d.setMode(static_cast<Mode>(three(rng)));
            d.setStereo(static_cast<Stereo>(three(rng)));
            d.setTimeMs(k % 71 == 0 ? std::nanf("") : ms(rng));
            d.setSync(static_cast<SyncDivision>(div(rng)));
            d.setTempoBpm(k % 43 == 0 ? std::nan("") : bpm(rng));
            d.setFeedbackPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            d.setMixPct(k % 59 == 0 ? std::nanf("") : pct(rng));
            d.setLowCutHz(hz(rng));
            d.setHighCutHz(k % 67 == 0 ? INFINITY : hz(rng));
            d.setModDepthPct(pct(rng));
            d.setSaturationPct(pct(rng));
            d.setDuckDb(k % 53 == 0 ? std::nanf("") : db(rng));
            d.setEnabled(coin(rng) == 1 || k % 5 != 0);
            d.setSoftBypass(k % 7 == 0);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
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
