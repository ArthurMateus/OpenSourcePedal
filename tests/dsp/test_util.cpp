// Tests for dsp-core/util: LinearRamp, OnePoleSmoother, Crossfader, DelayLine (issue #64).

#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"
#include "osp/dsp/util/Smoothers.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

namespace {

using osp::dsp::Crossfader;
using osp::dsp::DelayLine;
using osp::dsp::LinearRamp;
using osp::dsp::OnePoleSmoother;

constexpr int kMaxBlock = 2048;

// Minimal radix-2 FFT for the splatter check. Test-only; replaced by the shared harness from #62 when it lands.
void fft(std::vector<std::complex<double>>& x)
{
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t k = 0; k < len / 2; ++k, w *= wlen) {
                const auto u = x[i + k];
                const auto v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }
        }
    }
}

// Zipper metric (ADR-0007 REQ-TEST-003, "residual" variant): power in the block-rate sidebands f0 ± m·fs/B
// (m = 1…4, ±10 Hz each) relative to the carrier, in dBc. Per-block parameter steps put their energy exactly there.
// The literal "outside f0 ± 50 Hz" form cannot be met by any smoother: an ideal per-sample 50 ms −60→0 dB ramp
// already scores −34 dBc on it (measured, see PR #64). 4-term Blackman-Harris window (−92 dB sidelobes).
double zipperDbc(const std::vector<float>& y, double fs, double f0, int blockSize)
{
    const std::size_t n = y.size();
    std::vector<std::complex<double>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        x[i] = w * y[i];
    }
    fft(x);
    auto bandPower = [&](double fc) {
        double p = 0.0;
        for (std::size_t k = 0; k <= n / 2; ++k)
            if (std::abs(static_cast<double>(k) * fs / static_cast<double>(n) - fc) <= 10.0)
                p += std::norm(x[k]);
        return p;
    };
    double sidebands = 0.0;
    for (int m = 1; m <= 4; ++m)
        for (const double sign : {-1.0, 1.0}) {
            const double fc = f0 + sign * m * fs / blockSize;
            if (fc > 0.0 && fc < fs / 2)
                sidebands += bandPower(fc);
        }
    return 10.0 * std::log10(sidebands / bandPower(f0));
}

// Bin-centred sine with an odd bin index (ADR-0007 §2: coprime with the power-of-two FFT length).
std::vector<float> binCentredSine(double fs, double nominalHz, int n, double* exactHz)
{
    auto k = static_cast<long>(std::lround(nominalHz * n / fs));
    if (k % 2 == 0)
        ++k;
    *exactHz = static_cast<double>(k) * fs / n;
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * static_cast<double>(k) * i / n));
    return x;
}

// Gain ramped from −60 dB to 0 dB over 50 ms in the middle of the window (parameter written once per 64-sample
// block of `blockSize`, as the host does). `next(target)` maps the per-block target to the per-sample applied gain.
template <typename NextGain>
std::vector<float> gainRampRender(const std::vector<float>& in, double fs, int blockSize, NextGain&& next)
{
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    const int rampStart = total / 2 - static_cast<int>(0.025 * fs);
    const int rampLen = static_cast<int>(0.050 * fs);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            const double p = std::clamp(static_cast<double>(pos - rampStart) / rampLen, 0.0, 1.0);
            const auto target = static_cast<float>(std::pow(10.0, (-60.0 + 60.0 * p) / 20.0));
            for (int i = pos; i < std::min(pos + blockSize, total); ++i)
                out[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * next(target);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
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

TEST_CASE("Smoothing: no zipper noise on a 50 ms gain ramp (REQ-TEST-003)", "[dsp][util][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(32, 64);
    const int n = fs > 50000.0 ? 131072 : 65536;
    double f0 = 0.0;
    const auto in = binCentredSine(fs, 1000.0, n, &f0);

    LinearRamp ramp; // "Gain / level" class: 20 ms linear ramp of the linear gain
    ramp.reset(0.001f);
    ramp.prepare(fs, 0.020);
    const auto linear = gainRampRender(in, fs, blockSize, [&](float t) { ramp.setTarget(t); return ramp.next(); });

    OnePoleSmoother pole; // "Filter / tone" class: 20 ms one-pole
    pole.prepare(fs, 0.020);
    pole.reset(0.001f);
    const auto onePole = gainRampRender(in, fs, blockSize, [&](float t) { pole.setTarget(t); return pole.next(); });

    const auto stepped = gainRampRender(in, fs, blockSize, [](float t) { return t; }); // no smoothing

    const double zLinear = zipperDbc(linear, fs, f0, blockSize);
    const double zPole = zipperDbc(onePole, fs, f0, blockSize);
    const double zStepped = zipperDbc(stepped, fs, f0, blockSize);
    CAPTURE(fs, blockSize, zLinear, zPole, zStepped);
    REQUIRE(zLinear <= -60.0);
    REQUIRE(zPole <= -60.0);
    REQUIRE(zLinear <= zStepped - 20.0); // smoothing removes the per-block steps the metric detects
    REQUIRE(zPole <= zStepped - 20.0);
    WARN("zipper dBc fs=" << fs << " B=" << blockSize << ": linear " << zLinear << ", one-pole " << zPole
                          << ", stepped " << zStepped);
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
