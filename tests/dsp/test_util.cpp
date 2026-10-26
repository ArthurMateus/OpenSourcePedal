// Tests for dsp-core/util: LinearRamp, OnePoleSmoother, Crossfader, DelayLine (issue #64).

#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"
#include "osp/dsp/util/Smoothers.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace {

using osp::dsp::Crossfader;
using osp::dsp::DelayLine;
using osp::dsp::LinearRamp;
using osp::dsp::OnePoleSmoother;

constexpr int kMaxBlock = 2048;

// REQ-TEST-003 (ADR-0007, amended by #81): a step of the smoothed parameter at a non-block-aligned sample on a
// 1 kHz sine. Gate: RMS residual against a per-sample double-precision reference of the smoothing law, relative to
// the steady-state output RMS, ≤ −60 dBc.
constexpr int kStepAt = 30'001; // not a multiple of any tested block size

std::vector<double> sine1k(double fs, int n)
{
    std::vector<double> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / fs);
    return x;
}

// Applies the per-sample gain law `ref(i)` (double) and the primitive under test `next()` (float, driven in blocks
// of `blockSize`, with the target written exactly at kStepAt) to the same sine, and returns the residual in dBc.
template <typename Ref, typename Next, typename Step>
double residualDbc(double fs, int blockSize, Ref&& ref, Next&& next, Step&& step)
{
    const int n = kStepAt + static_cast<int>(0.2 * fs);
    const auto x = sine1k(fs, n);
    std::vector<float> y(static_cast<std::size_t>(n));
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < n; pos += blockSize) {
            for (int i = pos; i < std::min(pos + blockSize, n); ++i) {
                if (i == kStepAt)
                    step();
                y[static_cast<std::size_t>(i)] = static_cast<float>(x[static_cast<std::size_t>(i)]) * next();
            }
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    double err = 0.0, sig = 0.0;
    for (int i = 0; i < n; ++i) {
        const double r = x[static_cast<std::size_t>(i)] * ref(i);
        err += (y[static_cast<std::size_t>(i)] - r) * (y[static_cast<std::size_t>(i)] - r);
    }
    const double steady = std::max(std::abs(ref(0)), std::abs(ref(n - 1))); // larger of the two settled gains
    sig = 0.125 * steady * steady * n;                                       // RMS² of 0.5·steady·sine
    return 10.0 * std::log10(err / sig);
}

} // namespace

TEST_CASE("LinearRamp: length, exact landing, no jump on retarget", "[dsp][util]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    LinearRamp r;
    r.reset(1.0f);
    r.prepare(fs, 0.02);
    REQUIRE(r.lengthSamples() == static_cast<int>(std::lround(fs * 0.02)));

    r.setTarget(0.25f);
    float prev = r.current();
    for (int i = 0; i < r.lengthSamples() / 2; ++i) {
        const float v = r.next();
        REQUIRE(v <= prev);
        prev = v;
    }
    r.setTarget(2.0f); // reverse mid-ramp: continues from the current value
    const float first = r.next();
    REQUIRE(std::abs(first - prev) <= 2.0f / static_cast<float>(r.lengthSamples()));
    for (int i = 1; i < r.lengthSamples(); ++i)
        r.next();
    REQUIRE(r.current() == 2.0f);
    REQUIRE_FALSE(r.isRamping());
}

TEST_CASE("OnePoleSmoother: tau is the 63 % time, settles exactly", "[dsp][util]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double tau = 0.020;
    OnePoleSmoother s;
    s.prepare(fs, tau);
    s.setTarget(1.0f);
    const int tauSamples = static_cast<int>(std::lround(tau * fs));
    float v = 0.0f;
    for (int i = 0; i < tauSamples; ++i)
        v = s.next();
    REQUIRE(std::abs(v - (1.0f - std::exp(-1.0f))) <= 0.002f);

    std::vector<float> tail(static_cast<std::size_t>(20 * tauSamples));
    for (auto& t : tail)
        t = s.next();
    REQUIRE(s.current() == 1.0f);
    REQUIRE_FALSE(s.isSmoothing());
    OSP_REQUIRE_OUTPUT_CLEAN(tail);

    OnePoleSmoother unprepared; // before prepare(): targets apply immediately
    unprepared.setTarget(0.5f);
    REQUIRE(unprepared.next() == 0.5f);
}

TEST_CASE("Crossfader: 10 ms, constant amplitude, exact end states", "[dsp][util]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, blockSize);

    Crossfader xf;
    xf.prepare(fs);
    REQUIRE(xf.lengthSamples() == static_cast<int>(std::lround(fs * Crossfader::kBlockEnableSeconds)));
    REQUIRE(xf.isSettledOnA());

    const int total = 3 * xf.lengthSamples();
    std::vector<float> a(static_cast<std::size_t>(total)), b(a.size()), same(a.size()), out(a.size());
    for (int i = 0; i < total; ++i) {
        a[static_cast<std::size_t>(i)] = std::sin(0.013f * static_cast<float>(i));
        b[static_cast<std::size_t>(i)] = 0.3f * std::cos(0.007f * static_cast<float>(i)) - 0.2f;
    }

    // Identical inputs: the fade is inaudible (gains sum to 1 at every sample).
    xf.setTarget(true);
    const auto rt1 = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            const int n = std::min(blockSize, total - pos);
            xf.process(a.data() + pos, a.data() + pos, same.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt1);
    for (int i = 0; i < total; ++i)
        REQUIRE(std::abs(same[static_cast<std::size_t>(i)] - a[static_cast<std::size_t>(i)]) <= 1e-6f);

    // Different inputs: exactly a before, exactly b after the fade length.
    xf.reset(false);
    xf.setTarget(true);
    const auto rt2 = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            const int n = std::min(blockSize, total - pos);
            xf.process(a.data() + pos, b.data() + pos, out.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt2);
    for (int i = xf.lengthSamples(); i < total; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == b[static_cast<std::size_t>(i)]);
    REQUIRE_FALSE(xf.isFading());
    REQUIRE(xf.target());
}

TEST_CASE("DelayLine: exact integer delay, in place, block-size invariant, reset", "[dsp][util]")
{
    const int delay = GENERATE(0, 1, 17, 255);
    const int blockSize = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(delay, blockSize);

    DelayLine d;
    std::vector<float> pre(8, 1.0f);
    d.process(pre.data(), pre.data(), 8); // before prepare(): zero output
    REQUIRE(std::all_of(pre.begin(), pre.end(), [](float v) { return v == 0.0f; }));

    d.prepare(255);
    d.setDelay(delay);
    REQUIRE(d.getLatencySamples() == delay);

    const int total = 5000;
    std::vector<float> x(static_cast<std::size_t>(total));
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : x)
        v = dist(rng);

    auto render = [&] {
        std::vector<float> y = x; // processed in place
        const int step = blockSize > 0 ? blockSize : 64;
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += step) {
                if (blockSize == 0)
                    d.process(y.data() + pos, y.data() + pos, 0);
                d.process(y.data() + pos, y.data() + pos, std::min(step, total - pos));
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        return y;
    };

    const auto y1 = render();
    for (int i = 0; i < total; ++i)
        REQUIRE(y1[static_cast<std::size_t>(i)] == (i < delay ? 0.0f : x[static_cast<std::size_t>(i - delay)]));
    d.reset();
    REQUIRE(render() == y1);

    d.setDelay(10'000); // clamped to the prepared maximum
    REQUIRE(d.getDelay() == 255);
}

TEST_CASE("Smoothing: residual vs per-sample reference <= -60 dBc (REQ-TEST-003)", "[dsp][util][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(1, 32, 64, 127, 512);
    const bool up = GENERATE(true, false); // min→max and max→min
    CAPTURE(fs, blockSize, up);
    const double lo = 0.001, hi = 1.0; // −60 dB → 0 dB
    const double from = up ? lo : hi, to = up ? hi : lo;

    // LinearRamp, 20 ms ("Gain / level"): g = from + (to − from)·min(1, (i − s + 1) / L).
    LinearRamp ramp;
    ramp.reset(static_cast<float>(from));
    ramp.prepare(fs, 0.020);
    const int len = ramp.lengthSamples();
    const double rLinear = residualDbc(
        fs, blockSize,
        [&](int i) { return i < kStepAt ? from : from + (to - from) * std::min(1.0, double(i - kStepAt + 1) / len); },
        [&] { return ramp.next(); }, [&] { ramp.setTarget(static_cast<float>(to)); });

    // OnePoleSmoother, τ = 20 ms ("Filter / tone"): g = to + (from − to)·a^(i − s + 1).
    OnePoleSmoother pole;
    pole.prepare(fs, 0.020);
    pole.reset(static_cast<float>(from));
    const double a = std::exp(-1.0 / (0.020 * fs));
    const double rPole = residualDbc(
        fs, blockSize, [&](int i) { return i < kStepAt ? from : to + (from - to) * std::pow(a, i - kStepAt + 1); },
        [&] { return pole.next(); }, [&] { pole.setTarget(static_cast<float>(to)); });

    // Crossfader, 10 ms: the same law as LinearRamp between the two gains.
    Crossfader xf;
    xf.prepare(fs);
    xf.reset(false);
    const int xlen = xf.lengthSamples();
    float ga = static_cast<float>(from), gb = static_cast<float>(to), unit = 1.0f, mixed = 0.0f;
    const double rXfade = residualDbc(
        fs, blockSize,
        [&](int i) { return i < kStepAt ? from : from + (to - from) * std::min(1.0, double(i - kStepAt + 1) / xlen); },
        [&] {
            const float pa = ga * unit, pb = gb * unit;
            xf.process(&pa, &pb, &mixed, 1);
            return mixed;
        },
        [&] { xf.setTarget(true); });

    // Negative control: an unsmoothed step measured against the 20 ms linear law must fail the gate.
    float raw = static_cast<float>(from);
    const double rRaw = residualDbc(
        fs, blockSize,
        [&](int i) { return i < kStepAt ? from : from + (to - from) * std::min(1.0, double(i - kStepAt + 1) / len); },
        [&] { return raw; }, [&] { raw = static_cast<float>(to); });

    CAPTURE(rLinear, rPole, rXfade, rRaw);
    REQUIRE(rLinear <= -60.0);
    REQUIRE(rPole <= -60.0);
    REQUIRE(rXfade <= -60.0);
    REQUIRE(rRaw > -60.0);
    if (blockSize == 64)
        WARN("REQ-TEST-003 residual dBc fs=" << fs << (up ? " up" : " down") << ": linear " << rLinear << ", one-pole "
                                             << rPole << ", crossfade " << rXfade << ", unsmoothed " << rRaw);
}

TEST_CASE("util: RT checker with 1000 random parameter vectors, NaN/subnormal scan", "[dsp][util][rt]")
{
    constexpr double fs = 48000.0;
    constexpr int kBlock = 64;
    LinearRamp ramp;
    OnePoleSmoother pole;
    Crossfader xf;
    DelayLine d;
    ramp.prepare(fs, 0.02);
    pole.prepare(fs, 0.02);
    xf.prepare(fs);
    d.prepare(kMaxBlock);

    std::mt19937 rng(64u);
    std::uniform_real_distribution<float> val(-10.0f, 10.0f);
    std::uniform_int_distribution<int> delay(0, kMaxBlock);
    std::vector<float> params(3000);
    std::vector<int> delays(1000);
    for (auto& p : params)
        p = val(rng);
    for (auto& v : delays)
        v = delay(rng);

    // An impulse followed by 10 s of silence, so decaying states are checked for subnormals.
    std::vector<float> in(static_cast<std::size_t>(fs * 10.5), 0.0f), wet(in.size()), out(in.size());
    in[0] = 1.0f;
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0, blk = 0; pos < total; pos += kBlock, ++blk) {
            const int m = std::min(kBlock, total - pos);
            if (blk < 1000) {
                const auto k = static_cast<std::size_t>(blk) * 3;
                ramp.setTarget(params[k]);
                pole.setTarget(params[k + 1]);
                xf.setTarget(params[k + 2] > 0.0f);
                d.setDelay(delays[static_cast<std::size_t>(blk)]);
            } else if (blk == 1000) {
                ramp.setTarget(0.0f);
                pole.setTarget(0.0f);
            }
            for (int i = pos; i < pos + m; ++i)
                wet[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * ramp.next() + pole.next();
            d.process(wet.data() + pos, wet.data() + pos, m);
            xf.process(in.data() + pos, wet.data() + pos, out.data() + pos, m);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
}
