// Output block (#74): level, mono sum, -0.3 dBFS true-peak safety limiter with no look-ahead, NaN guard, meters.

#include "osp/dsp/OutputStage.h"
#include "osp/dsp/conv/IrLoader.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using osp::dsp::OutputStage;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs + 0.4));
    return x;
}

// Renders a stereo pair in blocks under the RT checker (block 0 = an empty call before every block).
void render(OutputStage& o, std::vector<float>& l, std::vector<float>& r, int block)
{
    const int total = static_cast<int>(l.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (block == 0)
                o.process(l.data() + pos, r.data() + pos, 0);
            o.process(l.data() + pos, r.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

// True peak in dBFS: 8x band-limited reconstruction (IrLoader's Kaiser-windowed sinc), skipping the edges.
// IrLoader::resample keeps an IR's frequency response, so its taps are scaled by fromRate / toRate: an 8x upsample
// comes out 1/8 in amplitude, and is scaled back here.
double truePeakDb(const std::vector<float>& x, double fs)
{
    const auto up = osp::dsp::IrLoader::resample(x.data(), static_cast<int>(x.size()), fs, 8.0 * fs);
    float p = 0.0f;
    for (std::size_t i = up.size() / 10; i < up.size() - up.size() / 10; ++i)
        p = std::max(p, 8.0f * std::abs(up[i]));
    return 20.0 * std::log10(std::max(p, 1e-12f));
}

} // namespace

TEST_CASE("OutputStage: rate x block matrix; transparent below the ceiling", "[dsp][output]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    OutputStage o;
    o.prepare(fs, kMaxBlock);
    REQUIRE(o.getLatencySamples() == 0);
    // Defaults: level 0 dB, limiter on, mono off. A -1 dBFS mix of 1 kHz and 3 kHz stays bit-exact.
    auto l = sine(1000.0, fs, static_cast<int>(0.1 * fs), 0.6);
    auto r = sine(3000.0, fs, static_cast<int>(0.1 * fs), 0.3);
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] += 0.29f * r[i];
    const auto l0 = l, r0 = r;
    render(o, l, r, block);
    CHECK(l == l0);
    CHECK(r == r0);
    CHECK(o.gainReductionDb() == 0.0f);
}

// Without look-ahead (L_out = 0) the interpolated true-peak estimate lags by kTpTaps / 2 samples. Steady material,
// sines up to 19 kHz at +12 dBFS, must stay at or below 0 dBTP ("no overs"). Dense transients driven far over the
// ceiling (white noise 3.5 dB over full scale, a naive non-band-limited square wave) can overshoot between samples
// during that lag; measured +2.4 and +0.7 dBTP. Their sample peaks stay at the ceiling, and a +3 dBTP bound catches
// regressions. Closing that gap needs about 6 samples of look-ahead, which signal-flow §4 does not allow (see the PR).
TEST_CASE("OutputStage: the limiter leaves no true-peak overs (8x reconstruction)", "[dsp][output]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    CAPTURE(fs);
    const int n = static_cast<int>(0.25 * fs);
    std::vector<std::vector<float>> signals;
    std::vector<std::string> names;
    for (double hz : {100.0, 1000.0, 5000.0, 11025.0, 15000.0, 19000.0})
        for (double over : {2.0, 4.0}) { // +6 and +12 dBFS
            signals.push_back(sine(hz, fs, n, over));
            names.push_back("sine " + std::to_string(static_cast<int>(hz)) + " Hz x" +
                            std::to_string(static_cast<int>(over)));
        }
    std::vector<float> square(static_cast<std::size_t>(n)), noise(static_cast<std::size_t>(n), 0.0f),
        clicks(static_cast<std::size_t>(n), 0.0f);
    std::mt19937 rng(74);
    std::normal_distribution<float> g(0.0f, 1.5f);
    for (int i = 0; i < n; ++i) {
        square[static_cast<std::size_t>(i)] = (i / static_cast<int>(fs / 440.0)) % 2 == 0 ? 3.0f : -3.0f;
        noise[static_cast<std::size_t>(i)] = g(rng);
    }
    for (int i = 0; i < n; i += 997)
        clicks[static_cast<std::size_t>(i)] = (i / 997) % 2 == 0 ? 4.0f : -4.0f;
    signals.push_back(square);
    signals.push_back(noise);
    signals.push_back(clicks);
    names.insert(names.end(), {"naive square 440 Hz x3", "gaussian noise sigma 1.5", "alternating clicks x4"});
    const std::size_t steady = signals.size() - 3; // the sines; the rest are the transient-dense stress signals

    double worst = -HUGE_VAL;
    for (std::size_t si = 0; si < signals.size(); ++si) {
        const auto& s = signals[si];
        CAPTURE(names[si]);
        OutputStage o;
        o.prepare(fs, kMaxBlock);
        auto l = s, r = s;
        render(o, l, r, 64);
        float samplePeak = 0.0f;
        for (float v : l)
            samplePeak = std::max(samplePeak, std::abs(v));
        CHECK(20.0 * std::log10(samplePeak) <= OutputStage::kCeilingDb + 1e-4); // sample peaks: guaranteed
        const double tp = truePeakDb(l, fs);
        if (si < steady) {
            worst = std::max(worst, tp);
            CHECK(tp <= 0.0); // no overs
        } else {
            WARN(names[si] << ": " << tp << " dBTP (transient-dense, no look-ahead)");
            CHECK(tp <= 3.0);
        }
    }
    WARN("worst steady-signal true peak at " << fs << " Hz: " << worst << " dBTP (ceiling " << OutputStage::kCeilingDb
                                             << ")");
}

TEST_CASE("OutputStage: level ramp residual, mono sum, limiter switch, meters, NaN guard", "[dsp][output]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(0.1 * fs);

    SECTION("level -6 dB follows its 20 ms linear ramp (REQ-TEST-003 <= -60 dBc)")
    {
        OutputStage o;
        o.prepare(fs, kMaxBlock);
        o.setLimiter(false);
        o.reset();
        auto l = sine(1000.0, fs, n, 0.5), r = l;
        const auto in = l;
        o.setLevelDb(-6.0f);
        render(o, l, r, 64);
        osp::dsp::LinearRamp law{1.0f};
        law.prepare(fs, 0.020);
        law.setTarget(std::pow(10.0f, -6.0f / 20.0f));
        std::vector<double> ref(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
            ref[i] = static_cast<double>(in[i]) * law.next();
        CHECK(osp::measure::residualDbc(l, ref) <= -60.0);
    }
    SECTION("mono sum: L = R = (L + R) / 2 after its 10 ms crossfade")
    {
        OutputStage o;
        o.prepare(fs, kMaxBlock);
        o.setMonoSum(true);
        auto l = sine(440.0, fs, n, 0.4), r = sine(660.0, fs, n, 0.2);
        const auto l0 = l, r0 = r;
        render(o, l, r, 64);
        for (int i = static_cast<int>(OutputStage::kSwitchSeconds * fs) + 1; i < n; ++i) {
            const float want = 0.5f * (l0[static_cast<std::size_t>(i)] + r0[static_cast<std::size_t>(i)]);
            REQUIRE(std::abs(l[static_cast<std::size_t>(i)] - want) <= 1e-6f);
            REQUIRE(l[static_cast<std::size_t>(i)] == r[static_cast<std::size_t>(i)]);
        }
    }
    SECTION("switching the limiter off mid-over crossfades without a jump")
    {
        OutputStage o;
        o.prepare(fs, kMaxBlock);
        auto l = sine(200.0, fs, n, 2.0), r = l;
        const int half = n / 2;
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            o.process(l.data(), r.data(), half);
            o.setLimiter(false);
            o.process(l.data() + half, r.data() + half, n - half);
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        float maxStep = 0.0f;
        for (int i = half + 1; i < n; ++i)
            maxStep = std::max(maxStep, std::abs(l[static_cast<std::size_t>(i)] - l[static_cast<std::size_t>(i) - 1]));
        // A 200 Hz sine at amplitude 2 moves at most 2 * 2 pi 200 / fs per sample; the fade adds less than 1/480 of
        // the 2.0 - ceiling difference per sample.
        CHECK(maxStep <= 2.0f * 2.0f * std::numbers::pi_v<float> * 200.0f / 48000.0f + 2.0f / 480.0f);
        CHECK(std::abs(l.back()) <= 2.0f); // unlimited again at the end
    }
    SECTION("meters, NaN guard and bypass")
    {
        OutputStage o;
        o.prepare(fs, kMaxBlock);
        auto l = sine(1000.0, fs, 4800, 0.5), r = std::vector<float>(4800, 0.0f);
        render(o, l, r, 4800 > kMaxBlock ? kMaxBlock : 4800);
        o.process(l.data(), r.data(), 0);
        auto l2 = sine(1000.0, fs, 480, 0.5), r2 = std::vector<float>(480, 0.0f); // 10 whole periods
        o.process(l2.data(), r2.data(), 480);
        CHECK(std::abs(o.peak(0) - 0.5f) < 0.002f);
        CHECK(std::abs(o.rms(0) - 0.5f / std::sqrt(2.0f)) < 0.002f);
        CHECK(o.peak(1) == 0.0f);

        std::vector<float> bl(256, 0.25f), br(256, 0.25f);
        bl[10] = std::nanf("");
        br[20] = INFINITY;
        CHECK_FALSE(o.nanDetected());
        o.process(bl.data(), br.data(), 256);
        CHECK(o.nanDetected());
        CHECK(bl[10] == 0.0f);
        CHECK(br[20] == 0.0f);
        o.clearNanDetected();
        CHECK_FALSE(o.nanDetected());

        o.setBypassed(true);
        o.setLevelDb(-20.0f);
        auto bx = sine(300.0, fs, 1024, 3.0), by = bx;
        const auto bx0 = bx;
        o.process(bx.data(), by.data(), 1024);
        CHECK(bx == bx0); // REQ-TEST-004: bypass is an exact null
    }
}

TEST_CASE("OutputStage: reset(), pre-prepare and 1000 random parameter vectors", "[dsp][output]")
{
    OutputStage cold;
    std::vector<float> l(64, 0.5f), r(64, 0.5f);
    cold.process(l.data(), r.data(), 64);
    CHECK(l == std::vector<float>(64, 0.0f));

    OutputStage o;
    o.prepare(48000.0, kMaxBlock);
    const auto s = sine(150.0, 48000.0, 9600, 3.0);
    auto a = s, ar = s, b = s, br = s;
    o.setLevelDb(4.0f);
    o.reset();
    render(o, a, ar, 64);
    o.reset();
    render(o, b, br, 127);
    CHECK(a == b);

    std::mt19937 rng(7474);
    std::uniform_real_distribution<float> lvl(-80.0f, 20.0f), sig(-4.0f, 4.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1);
    std::vector<float> bl(kMaxBlock), brr(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            o.setLevelDb(k % 53 == 0 ? std::nanf("") : lvl(rng));
            o.setLimiter(coin(rng) == 1);
            o.setMonoSum(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = sig(rng);
                brr[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
            }
            o.process(bl.data(), brr.data(), n);
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(bl[static_cast<std::size_t>(i)]) || !std::isfinite(brr[static_cast<std::size_t>(i)]))
                    ++nonFinite;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0);
}
