// Tests for the Dattorro plate tank (issue #72, part 3): T_mid RT60 vs the decay parameter, stereo decorrelation,
// echo density and tail flatness, freeze, block-size invariance, reset, and the RT fuzz with NaN/Inf parameters.

#include "osp/dsp/reverb/DattorroTank.h"

#include "measure.hpp"
#include "reverb_metrics.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {

using osp::dsp::DattorroTank;

struct Stereo {
    std::vector<float> l, r;
};

DattorroTank makePlate(double fs, float decayS, float mod = 0.2f)
{
    DattorroTank t;
    t.prepare(fs);
    t.setDecaySeconds(decayS);
    t.setModulation(mod);
    t.reset();
    return t;
}

Stereo render(DattorroTank& t, const std::vector<float>& inL, const std::vector<float>& inR, int blockSize)
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

Stereo impulseResponse(DattorroTank& t, double fs, double seconds)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * fs), 0.0f);
    x[0] = 1.0f;
    return render(t, x, x, 64);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t length)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + length; ++i)
        s += double(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(length));
}

} // namespace

// The plate's RT60 is specified for 1-20 s: below about 0.6 s it floors near 0.55 s (see DattorroTank.h).
TEST_CASE("DattorroTank: measured T_mid RT60 within 10 % of the decay parameter (1-20 s)", "[dsp][reverb][plate]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const float decay = GENERATE(1.0f, 2.5f, 8.0f, 15.0f);
    CAPTURE(fs, decay);
    DattorroTank t = makePlate(fs, decay);
    const auto y = impulseResponse(t, fs, 1.3 * decay + 0.8);
    const double measured = osp::test::tMid(y.l, y.r, fs);
    CAPTURE(measured);
    WARN("plate RT60 (T_mid) fs=" << fs << " decay " << decay << " s: measured " << measured << " s ("
                                  << 100.0 * (measured / decay - 1.0) << " %)");
    REQUIRE(std::abs(measured / decay - 1.0) <= 0.10);
}

TEST_CASE("DattorroTank: decorrelated stereo, diffuse tail, no gross comb", "[dsp][reverb][plate]")
{
    const double fs = 48000.0;
    DattorroTank t = makePlate(fs, 2.5f);
    const auto y = impulseResponse(t, fs, 1.3);
    const double rho = osp::test::correlation(y.l, y.r, static_cast<std::size_t>(0.05 * fs));
    const auto ned = osp::test::echoDensity(y.l, fs, 0.1, 0.3);
    const auto flat = osp::test::flatness(y.l, static_cast<std::size_t>(0.2 * fs), static_cast<std::size_t>(1.0 * fs),
                                          fs, 200.0, 5000.0);
    WARN("plate: L/R correlation " << rho << ", echo density mixing time " << ned.mixingMs << " ms / mean "
                                   << ned.meanLate << ", tail flatness local " << flat.local << " / global "
                                   << flat.global);
    REQUIRE(std::abs(rho) <= 0.2);
    const auto later = osp::test::echoDensity(y.l, fs, 0.6, 0.8);
    WARN("plate echo density 0.6-0.8 s: " << later.meanLate);
    // A single figure-eight loop builds density more slowly than the 8-line FDN (NED 0.66 at 0.1-0.3 s, 0.94 at
    // 0.6-0.8 s, measured); the gates pin that behaviour against regressions (audibility *unverified*).
    REQUIRE(ned.mixingMs > 0.0);
    REQUIRE(ned.mixingMs <= 500.0);
    REQUIRE(later.meanLate >= 0.9);
    REQUIRE(flat.local >= 0.75);
}

TEST_CASE("DattorroTank: freeze holds the tail losslessly and mutes the input", "[dsp][reverb][plate]")
{
    const double fs = 48000.0;
    DattorroTank t = makePlate(fs, 2.5f);
    const auto n = static_cast<std::size_t>(6.0 * fs);
    std::vector<float> in(n, 0.0f);
    std::mt19937 rng(73u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.5 * fs); ++i)
        in[i] = u(rng);
    for (std::size_t i = static_cast<std::size_t>(3.0 * fs); i < static_cast<std::size_t>(3.5 * fs); ++i)
        in[i] = u(rng);
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
    WARN("plate freeze: 1.0->2.5 s " << osp::measure::toDb(b / a) << " dB, 1.0->5.5 s " << osp::measure::toDb(c / a)
                                     << " dB");
    REQUIRE(std::abs(osp::measure::toDb(b / a)) <= 0.5);
    REQUIRE(std::abs(osp::measure::toDb(c / a)) <= 0.5);
}

TEST_CASE("DattorroTank: rate x block-size invariance, reset() bit-identical, pre-prepare", "[dsp][reverb][plate]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    std::vector<float> in(static_cast<std::size_t>(0.3 * fs), 0.0f);
    std::mt19937 rng(8u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < in.size() / 3; ++i)
        in[i] = u(rng);
    DattorroTank ref = makePlate(fs, 2.5f);
    const auto expected = render(ref, in, in, 64);
    for (int block : {1, 32, 127, 512, 2048}) {
        CAPTURE(fs, block);
        DattorroTank t = makePlate(fs, 2.5f);
        const auto y = render(t, in, in, block);
        REQUIRE(y.l == expected.l);
        REQUIRE(y.r == expected.r);
        t.reset();
        REQUIRE(render(t, in, in, block).l == expected.l);
    }
    DattorroTank unprepared;
    std::vector<float> z(16, 1.0f), zl(16, 1.0f), zr(16, 1.0f);
    unprepared.process(z.data(), z.data(), zl.data(), zr.data(), 16);
    REQUIRE(std::all_of(zl.begin(), zl.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(zr.begin(), zr.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("DattorroTank: RT checker, 1000 random parameter vectors (out of range, NaN, Inf), then silence",
          "[dsp][reverb][plate][rt]")
{
    const double fs = 48000.0;
    DattorroTank t = makePlate(fs, 2.5f);
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
