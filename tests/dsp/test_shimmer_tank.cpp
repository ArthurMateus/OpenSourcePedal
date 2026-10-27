// Tests for the shimmer path (issue #73): GrainShifter pitch accuracy (±5 cents, YIN median), the shifted interval in
// the ShimmerTank's tail, RT60 with the shimmer off, bounded energy at 100 % shimmer / 20 s decay / freeze, the
// interval change, block-size invariance, reset, and the RT fuzz with NaN/Inf parameters.

#include "osp/dsp/reverb/GrainShifter.h"
#include "osp/dsp/reverb/ShimmerTank.h"
#include "osp/dsp/tuner/PitchDetector.h"

#include "measure.hpp"
#include "reverb_metrics.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

namespace {

using osp::dsp::GrainShifter;
using osp::dsp::ShimmerTank;
using Interval = ShimmerTank::Interval;

struct Stereo {
    std::vector<float> l, r;
};

// Hann-windowed power of x[from, from + length) at hz (a single-bin DFT).
double powerAt(const std::vector<float>& x, std::size_t from, std::size_t length, double fs, double hz)
{
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(length));
        const double ph = 2.0 * std::numbers::pi * hz * double(from + i) / fs;
        re += w * x[from + i] * std::cos(ph);
        im -= w * x[from + i] * std::sin(ph);
    }
    return re * re + im * im;
}

// Median YIN pitch (tuner PitchDetector) over frames every 10 ms of x[from, to), in cents from `nominal`. A two-tap
// shifter's line spectrum scatters around the target (each tap's phase jumps at its zero-gain wrap), so a spectral
// peak is not its pitch; the period within the grains is.
double medianCents(const std::vector<float>& x, std::size_t from, std::size_t to, double fs, double nominal)
{
    osp::dsp::PitchDetector yin;
    yin.prepare(fs);
    const auto need = static_cast<std::size_t>(yin.requiredSamples());
    std::vector<double> cents;
    for (std::size_t end = from + need; end <= to; end += static_cast<std::size_t>(0.010 * fs)) {
        const auto r = yin.detect(x.data() + end - need, static_cast<int>(need));
        if (r.valid)
            cents.push_back(1200.0 * std::log2(r.hz / nominal));
    }
    REQUIRE(cents.size() >= 50);
    std::nth_element(cents.begin(), cents.begin() + static_cast<std::ptrdiff_t>(cents.size() / 2), cents.end());
    return cents[cents.size() / 2];
}

std::vector<float> sine(double fs, double hz, double seconds, float amp = 0.5f)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * fs));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * double(i) / fs));
    return x;
}

ShimmerTank makeShimmer(double fs, float decayS, float amount, Interval interval = Interval::OctUp)
{
    ShimmerTank t;
    t.prepare(fs);
    t.setDecaySeconds(decayS);
    t.setModulation(0.2f);
    t.setShimmer(amount);
    t.setInterval(interval);
    t.reset();
    return t;
}

Stereo render(ShimmerTank& t, const std::vector<float>& inL, const std::vector<float>& inR, int blockSize)
{
    Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};
    const int total = static_cast<int>(inL.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize)
            t.process(inL.data() + pos, inR.data() + pos, out.l.data() + pos, out.r.data() + pos,
                      std::min(blockSize, total - pos));
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out.l);
    OSP_REQUIRE_OUTPUT_CLEAN(out.r);
    return out;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t length)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + length; ++i)
        s += double(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(length));
}

} // namespace

TEST_CASE("GrainShifter: octUp / fifthUp / octDown within 5 cents", "[dsp][reverb][shimmer]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto interval = GENERATE(Interval::OctUp, Interval::FifthUp, Interval::OctDown);
    const double r = ShimmerTank::ratio(interval);
    CAPTURE(fs, r);
    GrainShifter g;
    g.prepare(fs, 0.040);
    g.setRatio(r);
    const auto x = sine(fs, 330.0, 1.5); // the shifted tones (165-660 Hz) sit inside the tuner's range
    std::vector<float> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        y[i] = g.process(x[i]);
    OSP_REQUIRE_OUTPUT_CLEAN(y);
    const double cents = medianCents(y, static_cast<std::size_t>(0.1 * fs), y.size(), fs, 330.0 * r);
    WARN("GrainShifter fs=" << fs << " ratio " << r << ": median pitch " << cents << " cents off");
    REQUIRE(std::abs(cents) <= 5.0);
}

TEST_CASE("ShimmerTank: the interval builds up in the tail; RT60 of the hall with the shimmer off",
          "[dsp][reverb][shimmer]")
{
    const double fs = 48000.0;
    const auto interval = GENERATE(Interval::OctUp, Interval::FifthUp, Interval::OctDown);
    const double target = 440.0 * ShimmerTank::ratio(interval);
    auto burst = sine(fs, 440.0, 2.0);
    std::fill(burst.begin() + static_cast<std::ptrdiff_t>(0.5 * fs), burst.end(), 0.0f);
    auto shiftedOverInput = [&](float amount) {
        ShimmerTank t = makeShimmer(fs, 2.5f, amount, interval);
        const auto y = render(t, burst, burst, 64);
        const auto from = static_cast<std::size_t>(0.7 * fs), len = static_cast<std::size_t>(1.0 * fs);
        return osp::measure::powerToDb(powerAt(y.l, from, len, fs, target) / powerAt(y.l, from, len, fs, 440.0));
    };
    const double off = shiftedOverInput(0.0f), on = shiftedOverInput(1.0f);
    WARN("shimmer " << target << " Hz vs 440 Hz in the tail: " << off << " dB at 0 %, " << on << " dB at 100 %");
    REQUIRE(on - off >= 15.0);

    ShimmerTank dry = makeShimmer(fs, 2.5f, 0.0f, interval);
    std::vector<float> imp(static_cast<std::size_t>(4.0 * fs), 0.0f);
    imp[0] = 1.0f;
    const auto ir = render(dry, imp, imp, 64);
    const double t60 = osp::test::tMid(ir.l, ir.r, fs);
    WARN("shimmer 0 %: T_mid " << t60 << " s at decay 2.5 s");
    REQUIRE(std::abs(t60 / 2.5 - 1.0) <= 0.10);
}

TEST_CASE("ShimmerTank: bounded energy at 100 % shimmer and 20 s decay, and under freeze", "[dsp][reverb][shimmer]")
{
    const double fs = 48000.0;
    const auto interval = GENERATE(Interval::OctUp, Interval::FifthUp, Interval::OctDown);
    const bool freeze = GENERATE(false, true);
    CAPTURE(static_cast<int>(interval), freeze);
    const auto n = static_cast<std::size_t>(16.0 * fs);
    std::vector<float> in(n, 0.0f);
    std::mt19937 rng(73u);
    std::uniform_real_distribution<float> u(-0.25f, 0.25f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(1.0 * fs); ++i)
        in[i] = u(rng);
    auto run = [&](float amount) {
        ShimmerTank t = makeShimmer(fs, 20.0f, amount, interval);
        Stereo y{std::vector<float>(n), std::vector<float>(n)};
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (std::size_t pos = 0; pos < n; pos += 64) {
                if (freeze && pos == static_cast<std::size_t>(1.2 * fs) / 64 * 64)
                    t.setFreeze(true);
                t.process(in.data() + pos, in.data() + pos, y.l.data() + pos, y.r.data() + pos, 64);
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(y.l);
        return y.l;
    };
    const auto on = run(1.0f), off = run(0.0f);
    const auto win = static_cast<std::size_t>(2.0 * fs);
    const double first = rms(on, win, win), plain = rms(off, win, win);
    double worst = 0.0, peak = 0.0;
    for (std::size_t from = 2 * win; from + win <= n; from += win)
        worst = std::max(worst, rms(on, from, win));
    for (float v : on)
        peak = std::max(peak, double(std::abs(v)));
    WARN("shimmer 100 %, 20 s" << (freeze ? ", frozen" : "") << ": RMS 2-4 s " << osp::measure::toDb(first / plain)
                               << " dB above the shimmer-off tank, loudest later 2 s window "
                               << osp::measure::toDb(worst / first) << " dB above 2-4 s, peak " << peak);
    REQUIRE(first <= 2.0 * plain);   // the shimmer adds at most +6 dB
    REQUIRE(worst <= 1.122 * first); // and the tail never climbs by more than +1 dB once the input stops
}

TEST_CASE("ShimmerTank: an interval change dips the return without a click", "[dsp][reverb][shimmer]")
{
    const double fs = 48000.0;
    ShimmerTank t = makeShimmer(fs, 4.0f, 0.8f, Interval::OctUp);
    const auto x = sine(fs, 330.0, 3.0, 0.3f);
    Stereo y{std::vector<float>(x.size()), std::vector<float>(x.size())};
    const auto at = static_cast<std::size_t>(1.5 * fs) / 64 * 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < x.size(); pos += 64) {
            if (pos == at)
                t.setInterval(Interval::OctDown);
            t.process(x.data() + pos, x.data() + pos, y.l.data() + pos, y.r.data() + pos, 64);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(y.l);
    auto maxStep = [&](std::size_t from, std::size_t to) {
        float m = 0.0f;
        for (std::size_t i = from + 1; i < to; ++i)
            m = std::max(m, std::abs(y.l[i] - y.l[i - 1]));
        return m;
    };
    const float before = maxStep(at - static_cast<std::size_t>(0.5 * fs), at);
    const float during = maxStep(at, at + static_cast<std::size_t>(0.1 * fs));
    WARN("interval change: max sample step " << during << " vs " << before << " before");
    REQUIRE(during <= 1.5f * before);
}

TEST_CASE("ShimmerTank: rate x block-size invariance, reset() bit-identical, pre-prepare", "[dsp][reverb][shimmer]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    std::vector<float> in(static_cast<std::size_t>(0.3 * fs), 0.0f);
    std::mt19937 rng(8u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < in.size() / 3; ++i)
        in[i] = u(rng);
    ShimmerTank ref = makeShimmer(fs, 2.5f, 0.7f);
    const auto expected = render(ref, in, in, 64);
    for (int block : {1, 31, 32, 127, 512, 2048}) {
        CAPTURE(fs, block);
        ShimmerTank t = makeShimmer(fs, 2.5f, 0.7f);
        const auto y = render(t, in, in, block);
        REQUIRE(y.l == expected.l);
        REQUIRE(y.r == expected.r);
        t.reset();
        REQUIRE(render(t, in, in, block).l == expected.l);
    }
    ShimmerTank unprepared;
    std::vector<float> z(16, 1.0f), zl(16, 1.0f), zr(16, 1.0f);
    unprepared.process(z.data(), z.data(), zl.data(), zr.data(), 16);
    REQUIRE(std::all_of(zl.begin(), zl.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(zr.begin(), zr.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("ShimmerTank: RT checker, 1000 random parameter vectors (out of range, NaN, Inf), then silence",
          "[dsp][reverb][shimmer][rt]")
{
    const double fs = 48000.0;
    ShimmerTank t = makeShimmer(fs, 2.5f, 0.5f);
    std::mt19937 rng(1001u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    auto wild = [&](float v) {
        const float pick = u(rng);
        return pick < 0.03f   ? std::numeric_limits<float>::quiet_NaN()
               : pick < 0.05f ? std::numeric_limits<float>::infinity()
               : pick < 0.07f ? -std::numeric_limits<float>::infinity()
                              : v;
    };
    std::vector<float> l(512), r(512), ol(512), orr(512), all;
    all.reserve(2 * 1000 * 512 + static_cast<std::size_t>(20.0 * fs));
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            t.setDecaySeconds(wild(-5.0f + 40.0f * u(rng)));
            t.setModulation(wild(-10.0f + 20.0f * u(rng)));
            t.setShimmer(wild(-1.0f + 3.0f * u(rng)));
            t.setInterval(static_cast<Interval>(static_cast<int>(u(rng) * 2.99f)));
            t.setFreeze(u(rng) < 0.1f);
            for (std::size_t i = 0; i < l.size(); ++i) {
                l[i] = 2.0f * u(rng) - 1.0f;
                r[i] = 2.0f * u(rng) - 1.0f;
            }
            const int n = 1 + static_cast<int>(u(rng) * 511.0f);
            t.process(l.data(), r.data(), ol.data(), orr.data(), n);
            all.insert(all.end(), ol.begin(), ol.begin() + n);
            all.insert(all.end(), orr.begin(), orr.begin() + n);
        }
        t.setFreeze(false);
        t.setDecaySeconds(0.3f);
        std::fill(l.begin(), l.end(), 0.0f);
        for (int b = 0; b < static_cast<int>(10.0 * fs) / 512; ++b) {
            t.process(l.data(), l.data(), ol.data(), orr.data(), 512);
            all.insert(all.end(), ol.begin(), ol.end());
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(all);
    REQUIRE(std::all_of(all.end() - 512, all.end(), [](float v) { return v == 0.0f; }));
}
