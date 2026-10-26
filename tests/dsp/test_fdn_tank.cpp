// Tests for the FDN reverb tank (issue #72, part 1): RT60 vs the decay parameter, stereo decorrelation, tail spectral
// flatness (no metallic ringing), freeze, block-size invariance, reset, and the RT fuzz.

#include "osp/dsp/reverb/FdnTank.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace {

using osp::dsp::FdnTank;

const FdnTank::Config kRoom{2, 30.0, 15.0, 45.0, 6.0, 0.6};
const FdnTank::Config kHall{4, 80.0, 35.0, 110.0, 12.0, 0.4};

struct Stereo {
    std::vector<float> l, r;
};

FdnTank makeTank(double fs, const FdnTank::Config& c, float decayS, float mod = 0.2f)
{
    FdnTank t;
    t.prepare(fs, c);
    t.setDecaySeconds(decayS);
    t.setModulation(mod);
    t.reset(); // settle the smoothers on the new targets
    return t;
}

// Renders in blocks of `blockSize` under the RT checker and scans the output.
Stereo render(FdnTank& t, const std::vector<float>& inL, const std::vector<float>& inR, int blockSize)
{
    Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};
    const int total = static_cast<int>(inL.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            const int n = std::min(blockSize, total - pos);
            t.process(inL.data() + pos, inR.data() + pos, out.l.data() + pos, out.r.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out.l);
    OSP_REQUIRE_OUTPUT_CLEAN(out.r);
    return out;
}

Stereo impulseResponse(FdnTank& t, double fs, double seconds)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * fs), 0.0f);
    x[0] = 1.0f;
    return render(t, x, x, 64);
}

// T30 from the Schroeder energy decay curve of L + R: RT60 = 2·(t(−35 dB) − t(−5 dB)).
double rt60(const Stereo& y, double fs)
{
    std::vector<double> edc(y.l.size() + 1, 0.0);
    for (std::size_t n = y.l.size(); n-- > 0;)
        edc[n] = edc[n + 1] + double(y.l[n]) * y.l[n] + double(y.r[n]) * y.r[n];
    auto crossing = [&](double db) {
        const double level = edc[0] * std::pow(10.0, db / 10.0);
        std::size_t n = 0;
        while (n < edc.size() && edc[n] > level)
            ++n;
        return static_cast<double>(n) / fs;
    };
    return 2.0 * (crossing(-35.0) - crossing(-5.0));
}

// Spectral flatness (geometric / arithmetic mean of power) of a Hann-windowed Welch average over [loHz, hiHz].
double flatness(const std::vector<float>& x, std::size_t from, std::size_t length, double fs, double loHz, double hiHz)
{
    constexpr std::size_t kFrame = 4096;
    std::vector<double> power(kFrame / 2 + 1, 0.0), frame(kFrame);
    for (std::size_t start = from; start + kFrame <= from + length; start += kFrame / 2) {
        for (std::size_t i = 0; i < kFrame; ++i)
            frame[i] = x[start + i] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / kFrame));
        const auto spec = osp::measure::realFft(frame);
        for (std::size_t k = 0; k < power.size(); ++k)
            power[k] += std::norm(spec[k]);
    }
    double logSum = 0.0, sum = 0.0;
    int count = 0;
    for (auto k = static_cast<std::size_t>(loHz * kFrame / fs); k <= static_cast<std::size_t>(hiHz * kFrame / fs); ++k) {
        logSum += std::log(power[k]);
        sum += power[k];
        ++count;
    }
    return std::exp(logSum / count) / (sum / count);
}

double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = from; i < a.size(); ++i) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return ab / std::sqrt(aa * bb);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t length)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + length; ++i)
        s += double(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(length));
}

} // namespace

TEST_CASE("FdnTank: measured RT60 within 10 % of the decay parameter", "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const bool hall = GENERATE(false, true);
    const float decay = GENERATE(0.3f, 1.0f, 2.5f, 8.0f);
    CAPTURE(fs, hall, decay);
    FdnTank t = makeTank(fs, hall ? kHall : kRoom, decay);
    const double measured = rt60(impulseResponse(t, fs, 1.3 * decay + 0.3), fs);
    CAPTURE(measured);
    REQUIRE(std::abs(measured / decay - 1.0) <= 0.10);
}

TEST_CASE("FdnTank: stereo outputs are decorrelated on a mono input", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const bool hall = GENERATE(false, true);
    CAPTURE(hall);
    FdnTank t = makeTank(fs, hall ? kHall : kRoom, 2.5f);
    const auto y = impulseResponse(t, fs, 2.0);
    const double rho = correlation(y.l, y.r, static_cast<std::size_t>(0.05 * fs)); // tail after 50 ms
    CAPTURE(rho);
    REQUIRE(std::abs(rho) <= 0.2);
}

TEST_CASE("FdnTank: tail spectrum is flat (no metallic ringing), and the metric detects a metallic tank",
          "[dsp][reverb]")
{
    const double fs = 48000.0;
    const auto from = static_cast<std::size_t>(0.2 * fs), length = static_cast<std::size_t>(1.0 * fs);
    auto tailFlatness = [&](const FdnTank::Config& c) {
        FdnTank t = makeTank(fs, c, 8.0f);
        const auto y = impulseResponse(t, fs, 1.3);
        return flatness(y.l, from, length, fs, 200.0, 5000.0);
    };
    const double room = tailFlatness(kRoom), hall = tailFlatness(kHall);
    // Control: no diffuser, short lines, no modulation. A comb-like tank whose tail has sparse spectral lines.
    const double metallic = tailFlatness({0, 0.0, 3.0, 4.0, 0.0, 0.4});
    WARN("tail spectral flatness (Welch, 200 Hz-5 kHz): room " << room << ", hall " << hall << ", metallic control "
                                                               << metallic);
    REQUIRE(room >= 0.8);
    REQUIRE(hall >= 0.8);
    REQUIRE(metallic < 0.5);
}

TEST_CASE("FdnTank: freeze holds the tail losslessly and mutes the input", "[dsp][reverb]")
{
    const double fs = 48000.0;
    FdnTank t = makeTank(fs, kHall, 2.5f);
    const auto n = static_cast<std::size_t>(6.0 * fs);
    std::vector<float> in(n, 0.0f);
    std::mt19937 rng(72u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.5 * fs); ++i)
        in[i] = u(rng);
    for (std::size_t i = static_cast<std::size_t>(3.0 * fs); i < static_cast<std::size_t>(3.5 * fs); ++i)
        in[i] = u(rng); // played while frozen: must not enter the tank

    Stereo y{std::vector<float>(n), std::vector<float>(n)};
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < n; pos += 64) {
            if (pos == static_cast<std::size_t>(0.6 * fs) / 64 * 64)
                t.setFreeze(true);
            t.process(in.data() + pos, in.data() + pos, y.l.data() + pos, y.r.data() + pos, 64);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    const auto win = static_cast<std::size_t>(0.5 * fs);
    const double a = rms(y.l, static_cast<std::size_t>(1.0 * fs), win);
    const double b = rms(y.l, static_cast<std::size_t>(2.5 * fs), win);
    const double c = rms(y.l, static_cast<std::size_t>(5.5 * fs), win);
    CAPTURE(a, b, c);
    REQUIRE(std::abs(osp::measure::toDb(b / a)) <= 0.5); // lossless while frozen (it would drop 36 dB at RT60 2.5 s)
    REQUIRE(std::abs(osp::measure::toDb(c / a)) <= 0.5); // and the input at 3.0-3.5 s added nothing
}

TEST_CASE("FdnTank: rate x block-size invariance, reset() bit-identical, pre-prepare", "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    std::vector<float> in(static_cast<std::size_t>(0.3 * fs), 0.0f);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < in.size() / 3; ++i)
        in[i] = u(rng);
    FdnTank ref = makeTank(fs, kHall, 2.5f);
    const auto expected = render(ref, in, in, 64);
    for (int block : {1, 32, 127, 512, 2048}) {
        CAPTURE(fs, block);
        FdnTank t = makeTank(fs, kHall, 2.5f);
        const auto y = render(t, in, in, block);
        REQUIRE(y.l == expected.l);
        REQUIRE(y.r == expected.r);
        t.reset();
        REQUIRE(render(t, in, in, block).l == expected.l);
    }

    FdnTank unprepared;
    std::vector<float> z(16, 1.0f), zl(16, 1.0f), zr(16, 1.0f);
    unprepared.process(z.data(), z.data(), zl.data(), zr.data(), 16);
    REQUIRE(std::all_of(zl.begin(), zl.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(zr.begin(), zr.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("FdnTank: RT checker, 1000 random parameter vectors, full-scale noise, then 10 s of silence",
          "[dsp][reverb][rt]")
{
    const double fs = 48000.0;
    FdnTank t = makeTank(fs, kHall, 2.5f);
    std::mt19937 rng(1000u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<float> l(512), r(512), ol(512), orr(512), all;
    all.reserve(1000 * 512 + static_cast<std::size_t>(10.0 * fs));
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            t.setDecaySeconds(-5.0f + 40.0f * u(rng)); // out of range on both sides
            t.setModulation(-0.2f + 1.4f * u(rng));
            t.setFreeze(u(rng) < 0.1f);
            for (std::size_t i = 0; i < l.size(); ++i) {
                l[i] = 2.0f * u(rng) - 1.0f;
                r[i] = 2.0f * u(rng) - 1.0f;
            }
            const int n = 1 + static_cast<int>(u(rng) * 511.0f);
            t.process(l.data(), r.data(), ol.data(), orr.data(), n);
            all.insert(all.end(), ol.begin(), ol.begin() + n);
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
    REQUIRE(std::all_of(all.end() - 512, all.end(), [](float v) { return v == 0.0f; })); // the tail died out
}
