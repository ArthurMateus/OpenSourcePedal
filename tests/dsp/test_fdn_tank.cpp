// Tests for the FDN reverb tank (issue #72, part 1): RT60 (T_mid) vs the decay parameter, stereo decorrelation, tail
// spectral flatness and echo density (no metallic ringing, fast diffusion), freeze, block-size invariance, reset, and
// the RT fuzz with out-of-range and NaN/Inf parameters.

#include "osp/dsp/reverb/FdnTank.h"

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

using osp::dsp::FdnTank;

constexpr FdnTank::Config kRoom = FdnTank::kRoom;
constexpr FdnTank::Config kHall = FdnTank::kHall;

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

double rms(const std::vector<float>& x, std::size_t from, std::size_t length)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + length; ++i)
        s += double(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(length));
}

} // namespace

TEST_CASE("FdnTank: measured T_mid RT60 within 10 % of the decay parameter", "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const bool hall = GENERATE(false, true);
    const float decay = GENERATE(0.3f, 1.0f, 2.5f, 8.0f);
    CAPTURE(fs, hall, decay);
    FdnTank t = makeTank(fs, hall ? kHall : kRoom, decay);
    const auto y = impulseResponse(t, fs, 1.3 * decay + 0.3);
    const double measured = osp::test::tMid(y.l, y.r, fs);
    CAPTURE(measured);
    WARN("RT60 (T_mid: mean of the 500 Hz and 1 kHz octave T30s) fs=" << fs << (hall ? " hall" : " room") << " decay "
                                                                      << decay << " s: measured " << measured << " s ("
                                                                      << 100.0 * (measured / decay - 1.0) << " %)");
    REQUIRE(std::abs(measured / decay - 1.0) <= 0.10);
}

TEST_CASE("FdnTank: stereo outputs are decorrelated on a mono input", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const bool hall = GENERATE(false, true);
    CAPTURE(hall);
    FdnTank t = makeTank(fs, hall ? kHall : kRoom, 2.5f);
    const auto y = impulseResponse(t, fs, 2.0);
    const double rho = osp::test::correlation(y.l, y.r, static_cast<std::size_t>(0.05 * fs)); // tail after 50 ms
    CAPTURE(rho);
    WARN((hall ? "hall" : "room") << " L/R correlation of the tail (mono impulse in): " << rho);
    REQUIRE(std::abs(rho) <= 0.2);
}

TEST_CASE("FdnTank: no metallic ringing (local spectral flatness of the tail); the metric flags a metallic tank",
          "[dsp][reverb]")
{
    const double fs = 48000.0;
    const float decay = GENERATE(1.0f, 2.5f, 8.0f, 20.0f);
    const auto from = static_cast<std::size_t>(0.2 * fs), length = static_cast<std::size_t>(1.0 * fs);
    auto tail = [&](const FdnTank::Config& c) {
        FdnTank t = makeTank(fs, c, decay);
        const auto y = impulseResponse(t, fs, 1.3);
        return osp::test::flatness(y.l, from, length, fs, 200.0, 5000.0);
    };
    const auto room = tail(kRoom), hall = tail(kHall);
    // Control: no diffuser, 3-4 ms lines, no modulation, a comb-like tank whose tail is a sparse set of lines. It
    // proves only that a gross comb is detected; slower diffusion is the echo-density test's job (the next test case).
    const auto metallic = tail({0, 0.0, 3.0, 4.0, 0.0, 0.4});
    WARN("tail flatness, decay " << decay << " s (Welch, 200 Hz-5 kHz, local / global): room " << room.local << " / "
                                 << room.global << ", hall " << hall.local << " / " << hall.global
                                 << ", metallic control " << metallic.local << " / " << metallic.global);
    REQUIRE(room.local >= 0.75);
    REQUIRE(hall.local >= 0.75);
    REQUIRE(metallic.local < 0.3);
}

TEST_CASE("FdnTank: the tail becomes diffuse fast (echo density); plausible weaker voicings are rejected",
          "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    CAPTURE(fs);
    auto density = [&](const FdnTank::Config& c, float mod) {
        FdnTank t = makeTank(fs, c, 2.5f, mod);
        const auto y = impulseResponse(t, fs, 0.6);
        return osp::test::echoDensity(y.l, fs, 0.1, 0.3);
    };
    const auto room = density(kRoom, 0.2f), hall = density(kHall, 0.2f);
    // Negatives: the 2-stage 15-45 ms room draft (PR #103 review) and the hall without its diffuser.
    const auto draft = density({2, 30.0, 15.0, 45.0, 6.0, 0.6}, 0.2f);
    const auto noDiffuser = density({0, 80.0, 35.0, 110.0, 12.0, 0.4}, 0.2f);
    WARN("echo density (Abel & Huang 2006), mixing time to 0.9 / mean over 100-300 ms: room "
         << room.mixingMs << " ms / " << room.meanLate << ", hall " << hall.mixingMs << " ms / " << hall.meanLate
         << ", room draft " << draft.mixingMs << " ms / " << draft.meanLate << ", hall without diffuser "
         << noDiffuser.mixingMs << " ms / " << noDiffuser.meanLate);
    REQUIRE(room.mixingMs > 0.0);
    REQUIRE(room.mixingMs <= 50.0);
    REQUIRE(hall.mixingMs > 0.0);
    REQUIRE(hall.mixingMs <= 80.0);
    REQUIRE(room.meanLate >= 0.9);
    REQUIRE(hall.meanLate >= 0.9);
    REQUIRE((draft.mixingMs < 0.0 || draft.mixingMs > 50.0)); // fails the room gate
    REQUIRE(noDiffuser.meanLate < 0.9); // fails the late-density gate
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
    WARN("freeze: tail level change 1.0->2.5 s " << osp::measure::toDb(b / a) << " dB, 1.0->5.5 s (input played at "
                                                 << "3.0-3.5 s) " << osp::measure::toDb(c / a) << " dB");
    REQUIRE(std::abs(osp::measure::toDb(b / a)) <= 0.5); // lossless while frozen (it would drop 36 dB at RT60 2.5 s)
    REQUIRE(std::abs(osp::measure::toDb(c / a)) <= 0.5); // and the input at 3.0-3.5 s added nothing
}

TEST_CASE("FdnTank: a 60 s freeze does not drift", "[dsp][reverb]")
{
    const double fs = 48000.0;
    FdnTank t = makeTank(fs, kHall, 2.5f);
    std::vector<float> in(64), l(64), r(64);
    std::mt19937 rng(60u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    double first = 0.0, last = 0.0;
    const int blocks = static_cast<int>(61.0 * fs) / 64, window = static_cast<int>(0.5 * fs) / 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int b = 0; b < blocks; ++b) {
            for (auto& v : in)
                v = b < window ? u(rng) : 0.0f; // 0.5 s of noise, frozen from 0.6 s on
            if (b == window + window / 5)
                t.setFreeze(true);
            t.process(in.data(), in.data(), l.data(), r.data(), 64);
            double e = 0.0;
            for (float v : l)
                e += double(v) * v;
            if (b >= 2 * window && b < 3 * window) // 1.0-1.5 s
                first += e;
            if (b >= blocks - window) // the last 0.5 s, about 60.5 s
                last += e;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    const double drift = osp::measure::powerToDb(last / first);
    WARN("freeze drift over 59 s: " << drift << " dB");
    REQUIRE(std::abs(drift) <= 0.5);
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

TEST_CASE("FdnTank: RT checker, 1000 random parameter vectors (out of range, NaN, Inf), then 10 s of silence",
          "[dsp][reverb][rt]")
{
    const double fs = 48000.0;
    FdnTank t = makeTank(fs, kHall, 2.5f);
    std::mt19937 rng(1000u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    auto wild = [&](float v) {
        const float pick = u(rng);
        return pick < 0.03f   ? std::numeric_limits<float>::quiet_NaN()
               : pick < 0.05f ? std::numeric_limits<float>::infinity()
               : pick < 0.07f ? -std::numeric_limits<float>::infinity()
                              : v;
    };
    std::vector<float> l(512), r(512), ol(512), orr(512), all;
    all.reserve(2 * 1000 * 512 + static_cast<std::size_t>(10.0 * fs));
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            t.setDecaySeconds(wild(-5.0f + 40.0f * u(rng))); // out of range on both sides, NaN, +-Inf
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
    REQUIRE(std::all_of(all.end() - 512, all.end(), [](float v) { return v == 0.0f; })); // the tail died out
}
