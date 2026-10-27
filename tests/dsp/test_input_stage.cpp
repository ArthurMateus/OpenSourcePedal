// Input block (#74): -24...+24 dB trim with a 20 ms ramp, then a 5 Hz DC blocker. Latency 0.

#include "osp/dsp/InputStage.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::InputStage;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp, double dc = 0.0)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] =
            static_cast<float>(dc + amp * std::sin(2.0 * std::numbers::pi * hz * i / fs + 0.2));
    return x;
}

std::vector<float> render(InputStage& s, const std::vector<float>& in, int block)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (block == 0)
                s.process(in.data() + pos, out.data() + pos, 0);
            s.process(in.data() + pos, out.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

double rmsDb(const std::vector<float>& x, std::size_t from)
{
    double s = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        s += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(s / static_cast<double>(x.size() - from));
}

} // namespace

TEST_CASE("InputStage: rate x block matrix, flat above 40 Hz, DC removed", "[dsp][input]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    InputStage s;
    s.prepare(fs, kMaxBlock);
    REQUIRE(s.getLatencySamples() == 0);
    // Low E with a 0.2 DC offset: the DC is gone after 1 s (> 5 time constants of 32 ms), the note within 0.05 dB.
    const auto in = sine(82.407, fs, static_cast<int>(1.5 * fs), 0.3, 0.2);
    const auto out = render(s, in, block);
    const auto settled = static_cast<std::size_t>(fs);
    double mean = 0.0;
    for (std::size_t i = settled; i < out.size(); ++i)
        mean += out[i];
    mean /= static_cast<double>(out.size() - settled);
    CHECK(std::abs(mean) < 1e-3);
    const auto ref = sine(82.407, fs, static_cast<int>(1.5 * fs), 0.3);
    CHECK(std::abs(rmsDb(out, settled) - rmsDb(ref, settled)) < 0.05);
}

TEST_CASE("InputStage: trim, clamp, ramp residual, bypass, reset, non-finite input", "[dsp][input]")
{
    const double fs = 48000.0;

    SECTION("+6 dB follows its 20 ms ramp through the same DC blocker (REQ-TEST-003 <= -60 dBc)")
    {
        InputStage s;
        s.prepare(fs, kMaxBlock);
        const auto in = sine(1000.0, fs, 9600, 0.25);
        s.setGainDb(6.0f);
        const auto out = render(s, in, 64);
        osp::dsp::LinearRamp law{1.0f};
        law.prepare(fs, 0.020);
        law.setTarget(std::pow(10.0f, 6.0f / 20.0f));
        osp::dsp::DcBlocker dc;
        dc.prepare(fs, InputStage::kDcCornerHz);
        std::vector<double> ref(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
            ref[i] = dc.tick(in[i] * law.next());
        CHECK(osp::measure::residualDbc(out, ref) <= -60.0);
    }
    SECTION("the trim is clamped to -24...+24 dB and NaN gives -24")
    {
        for (const auto [set, want] : {std::pair{40.0f, 24.0f}, {-40.0f, -24.0f}, {std::nanf(""), -24.0f}}) {
            InputStage s;
            s.prepare(fs, kMaxBlock);
            s.setGainDb(set);
            s.reset(); // jump to the target
            const auto in = sine(1000.0, fs, 4800, 0.01);
            const auto out = render(s, in, 64);
            CHECK(std::abs(rmsDb(out, 2400) - rmsDb(in, 2400) - want) < 0.05);
        }
    }
    SECTION("hard bypass is an exact null (REQ-TEST-004), and reset() makes renders bit-identical")
    {
        InputStage s;
        s.prepare(fs, kMaxBlock);
        s.setBypassed(true);
        s.setGainDb(12.0f);
        const auto in = sine(300.0, fs, 4096, 0.5, 0.1);
        CHECK(render(s, in, 127) == in);
        s.setBypassed(false);
        s.reset();
        const auto a = render(s, in, 64);
        s.reset();
        CHECK(render(s, in, 127) == a);
    }
    SECTION("before prepare() the output is silent; NaN/Inf input counts as 0")
    {
        InputStage cold;
        std::vector<float> in(64, 0.5f), out(64, 1.0f);
        cold.process(in.data(), out.data(), 64);
        CHECK(out == std::vector<float>(64, 0.0f));

        InputStage s;
        s.prepare(fs, kMaxBlock);
        auto bad = sine(500.0, fs, 4096, 0.3);
        bad[7] = std::nanf("");
        bad[900] = -INFINITY;
        const auto o = render(s, bad, 64); // OSP_REQUIRE_OUTPUT_CLEAN inside: all finite
        CHECK(std::isfinite(o.back()));
    }
}

TEST_CASE("InputStage: 1000 random parameter vectors under the RT checker", "[dsp][input]")
{
    InputStage s;
    s.prepare(96000.0, kMaxBlock);
    std::mt19937 rng(74);
    std::uniform_real_distribution<float> gain(-40.0f, 40.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock);
    std::vector<float> buf(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            s.setGainDb(k % 61 == 0 ? std::nanf("") : gain(rng));
            const int n = len(rng);
            for (int i = 0; i < n; ++i)
                buf[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? 1e-40f : sig(rng); // a subnormal too
            s.process(buf.data(), buf.data(), n);
            for (int i = 0; i < n; ++i)
                nonFinite += std::isfinite(buf[static_cast<std::size_t>(i)]) ? 0 : 1;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0);
}
