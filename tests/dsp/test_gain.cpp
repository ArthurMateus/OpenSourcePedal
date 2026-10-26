#include "osp/dsp/Gain.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace {

// ADR-0007 REQ-TEST-001: every rate x block size, with maxBlockSize = 2048.
constexpr int kMaxBlock = 2048;

std::vector<float> makeSignal(int length)
{
    std::vector<float> x(static_cast<std::size_t>(length));
    for (int n = 0; n < length; ++n)
        x[static_cast<std::size_t>(n)] = 0.25f * std::sin(0.01f * static_cast<float>(n)) + 0.5f; // stays in [0.25, 0.75]
    return x;
}

// Renders `in` through `gain` in blocks of `blockSize` (block size 0 is a no-op call before every real block).
// Every process() call runs under the RT checker (realtime-rules.md C1–C4, C8), and the output is scanned.
std::vector<float> render(osp::dsp::Gain& gain, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            if (blockSize == 0)
                gain.process(in.data() + pos, out.data() + pos, 0);
            gain.process(in.data() + pos, out.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

} // namespace

TEST_CASE("Gain: sample-rate x block-size matrix", "[dsp][gain]")
{
    const double sampleRate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(sampleRate, blockSize);

    osp::dsp::Gain gain;
    gain.prepare(sampleRate, kMaxBlock);
    REQUIRE(gain.getLatencySamples() == 0);

    const int rampSamples = static_cast<int>(std::lround(sampleRate * 0.02));
    const auto in = makeSignal(rampSamples * 3);

    SECTION("unity gain is bit-exact")
    {
        const auto out = render(gain, in, blockSize);
        REQUIRE(out == in);
    }

    SECTION("-6.02 dB settles to half amplitude after the ramp, without jumps")
    {
        gain.setGainDb(20.0f * std::log10(0.5f));
        const auto out = render(gain, in, blockSize);
        for (std::size_t n = static_cast<std::size_t>(rampSamples); n < in.size(); ++n)
            REQUIRE(std::abs(out[n] - 0.5f * in[n]) <= 1e-6f);
        for (std::size_t n = 0; n < in.size(); ++n)
            REQUIRE(std::isfinite(out[n]));
        // During the ramp the applied gain moves monotonically from 1 to 0.5.
        float previous = 1.0f;
        for (std::size_t n = 0; n < static_cast<std::size_t>(rampSamples); ++n) {
            const float applied = out[n] / in[n];
            REQUIRE(applied <= previous + 1e-5f);
            REQUIRE(applied >= 0.5f - 1e-5f);
            previous = applied;
        }
    }

    SECTION("reset() makes repeat renders bit-identical")
    {
        gain.setGainDb(-12.0f);
        gain.reset();
        const auto first = render(gain, in, blockSize);
        gain.setGainDb(+6.0f);
        (void)render(gain, in, blockSize); // disturb the state
        gain.setGainDb(-12.0f);
        gain.reset();
        const auto second = render(gain, in, blockSize);
        REQUIRE(first == second);
    }
}

TEST_CASE("Gain: process() before prepare() zero-fills and never reads past n", "[dsp][gain]")
{
    osp::dsp::Gain gain;
    std::vector<float> in(16, 1.0f), out(16, -1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { gain.process(in.data(), out.data(), 0); }));
    REQUIRE(out[0] == -1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { gain.process(in.data(), out.data(), 8); }));
    for (int i = 0; i < 8; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == 0.0f);
    REQUIRE(out[8] == -1.0f);
}

TEST_CASE("Gain: in-place processing equals out-of-place", "[dsp][gain]")
{
    osp::dsp::Gain a, b;
    a.prepare(48000.0, kMaxBlock);
    b.prepare(48000.0, kMaxBlock);
    a.setGainDb(-3.0f);
    b.setGainDb(-3.0f);
    auto buffer = makeSignal(4096);
    const auto reference = render(a, buffer, 127);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < 4096; pos += 127) {
            const int n = std::min(127, 4096 - pos);
            b.process(buffer.data() + pos, buffer.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    REQUIRE(buffer == reference);
}

TEST_CASE("Gain: RT checker, impulse + 10 s silence + 1000 parameter vectors (REQ-TEST-006)", "[dsp][gain][rt]")
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 64;
    osp::dsp::Gain gain;
    gain.prepare(kRate, kMaxBlock);

    // 0.5 s starting with a unit impulse, then 10 s of silence.
    std::vector<float> in(static_cast<std::size_t>(kRate * 10.5), 0.0f);
    in[0] = 1.0f;
    std::vector<float> out(in.size(), -1.0f);

    // 1000 parameter vectors from a fixed seed, one per block for the first 1000 blocks (generated outside the scope).
    std::mt19937 rng(45u);
    std::uniform_real_distribution<float> gainDb(-60.0f, 24.0f);
    std::vector<float> params(1000);
    for (auto& p : params)
        p = gainDb(rng);

    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0, block = 0; pos < total; pos += kBlock, ++block) {
            if (block < static_cast<int>(params.size()))
                gain.setGainDb(params[static_cast<std::size_t>(block)]);
            gain.process(in.data() + pos, out.data() + pos, std::min(kBlock, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
}
