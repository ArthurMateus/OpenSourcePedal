#include "osp/engine/Engine.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

namespace {

std::vector<float> ramp(int length, float offset)
{
    std::vector<float> x(static_cast<std::size_t>(length));
    for (int n = 0; n < length; ++n)
        x[static_cast<std::size_t>(n)] = offset + 0.001f * static_cast<float>(n % 500);
    return x;
}

std::vector<float> sine(double fs, int length, float amplitude)
{
    std::vector<float> x(static_cast<std::size_t>(length));
    for (int n = 0; n < length; ++n)
        x[static_cast<std::size_t>(n)] =
            amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * 997.0 * n / fs));
    return x;
}

// Host-sized blocks through processChunked (the app's call), under the RT checker.
void run(osp::engine::Engine& engine, std::vector<float>& left, std::vector<float>& right, int hostBlock)
{
    const int total = static_cast<int>(left.size());
    const int step = hostBlock > 0 ? hostBlock : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            float* channels[] = {left.data() + pos, right.data() + pos};
            if (hostBlock == 0)
                osp::engine::processChunked(engine, channels, 2, 0);
            osp::engine::processChunked(engine, channels, 2, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(left);
    OSP_REQUIRE_OUTPUT_CLEAN(right);
}

} // namespace

TEST_CASE("Engine: hard-bypassed graph is the mono input delayed by the reported latency, any host block", "[engine]")
{
    // ADR-0007 REQ-TEST-001: maxBlockSize 2048, and the app boundary also gets 2 x maxBlock.
    const double sampleRate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int hostBlock = GENERATE(0, 1, 32, 64, 127, 512, 2048, 4096);
    CAPTURE(sampleRate, hostBlock);

    osp::engine::Engine engine;
    engine.prepare(sampleRate, 2048);
    engine.graph().setBypassedAll(true);
    engine.reset();
    const auto latency = static_cast<std::size_t>(engine.getLatencySamples());
    REQUIRE(latency > 0); // OS-1

    auto left = ramp(9000, 0.1f), right = ramp(9000, -0.2f);
    const auto in = left;
    run(engine, left, right, hostBlock);
    for (std::size_t n = 0; n < in.size(); ++n) {
        const float expected = n >= latency ? in[n - latency] : 0.0f;
        REQUIRE(left[n] == expected);
        REQUIRE(right[n] == expected);
    }
}

TEST_CASE("Engine: reads the output level atomic once per block and ramps to it", "[engine]")
{
    const auto render = [](float levelDb) {
        std::atomic<float> level{levelDb};
        osp::engine::Engine engine;
        engine.setOutputLevelParameter(&level);
        engine.parameters().set("output.limiter", 0.0f);
        engine.prepare(48000.0, 64);
        auto left = sine(48000.0, 9600, 0.25f), right = left;
        run(engine, left, right, 64);
        return left;
    };
    const auto unity = render(0.0f), half = render(-6.0206f);
    for (std::size_t n = 4800; n < unity.size(); ++n) // past the 20 ms ramp
        REQUIRE(std::abs(half[n] - 0.5f * unity[n]) < 1e-4f);
}

TEST_CASE("Engine: reported latency is round(sum D) and matches the 1 kHz impulse delay (Moth's ask (a))", "[engine]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    osp::engine::Engine engine;
    engine.parameters().set("input.enabled", 0.0f);
    engine.parameters().set("output.limiter", 0.0f);
    engine.prepare(fs, 64);
    CHECK(engine.getLatencySamples() == std::lround(engine.graph().getMeasuredDelaySamples()));

    std::vector<float> left(4096, 0.0f), right(4096, 0.0f);
    left[0] = 0.5f;
    run(engine, left, right, 64);
    std::complex<double> num, den;
    const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
    for (std::size_t n = 0; n < left.size(); ++n) {
        const auto e = std::polar(static_cast<double>(left[n]), -w * static_cast<double>(n));
        num += static_cast<double>(n) * e;
        den += e;
    }
    CHECK(std::abs((num / den).real() - engine.getLatencySamples()) <= 0.5);
}

TEST_CASE("Engine: NaN/Inf/out-of-range fuzz of every parameter, every rate and host block (RT checker)",
          "[engine][fuzz]")
{
    const double sampleRate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int hostBlock = GENERATE(0, 1, 64, 127, 2048, 4096);
    CAPTURE(sampleRate, hostBlock);

    // Every entry bound to a raw atomic, so non-finite values reach the graph unsanitized.
    osp::engine::Engine engine;
    const auto table = osp::engine::paramTable();
    auto raw = std::make_unique<std::atomic<float>[]>(table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
        raw[i].store(table[i].def);
        engine.parameters().bind(static_cast<int>(i), &raw[i]);
    }
    engine.prepare(sampleRate, 2048);

    std::mt19937 rng(static_cast<unsigned>(sampleRate) + static_cast<unsigned>(hostBlock));
    std::uniform_int_distribution<std::size_t> pick(0, table.size() - 1);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    constexpr float kSpecial[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f};
    auto left = sine(sampleRate, 12000, 0.5f), right = left;
    const int total = static_cast<int>(left.size());
    const int step = hostBlock > 0 ? hostBlock : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            for (int k = 0; k < 8; ++k) { // 8 parameters per host block
                const std::size_t i = pick(rng);
                const float u = unit(rng);
                const float v = u < 0.2f ? kSpecial[static_cast<std::size_t>(u * 25.0f) % 5]
                                         : table[i].min + unit(rng) * (table[i].max - table[i].min);
                raw[i].store(v, std::memory_order_relaxed);
            }
            float* channels[] = {left.data() + pos, right.data() + pos};
            if (hostBlock == 0)
                osp::engine::processChunked(engine, channels, 2, 0);
            osp::engine::processChunked(engine, channels, 2, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(left);
    OSP_REQUIRE_OUTPUT_CLEAN(right);
    CHECK_FALSE(engine.graph().output().nanDetected());
}

TEST_CASE("Engine: process() before prepare() zero-fills", "[engine]")
{
    osp::engine::Engine engine;
    std::vector<float> x(32, 1.0f);
    float* channels[] = {x.data()};
    osp::engine::processChunked(engine, channels, 1, 32);
    for (float v : x)
        REQUIRE(v == 0.0f);
}
