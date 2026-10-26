#include "osp/dsp/conv/PartitionedConvolver.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using osp::dsp::PartitionedConvolver;

namespace {

std::vector<float> makeNoise(int length, unsigned seed, float amplitude)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    std::vector<float> x(static_cast<std::size_t>(length));
    for (auto& v : x)
        v = dist(rng);
    return x;
}

// Cab-like IR: noise under a 60 dB exponential decay over its length, scaled to unit energy.
std::vector<float> makeIr(int length, unsigned seed)
{
    auto h = makeNoise(length, seed, 1.0f);
    double energy = 0.0;
    for (int n = 0; n < length; ++n) {
        h[static_cast<std::size_t>(n)] *= std::pow(10.0f, -3.0f * static_cast<float>(n) / static_cast<float>(length));
        energy += double(h[static_cast<std::size_t>(n)]) * h[static_cast<std::size_t>(n)];
    }
    for (auto& v : h)
        v = static_cast<float>(v / std::sqrt(energy));
    return h;
}

// float64 direct-form reference (ADR-0007 §2) at every `stride`-th output; the other outputs are NaN (skipped).
std::vector<double> directConvolution(const std::vector<float>& x, const std::vector<float>& h, std::size_t stride = 1)
{
    std::vector<double> y(x.size(), std::nan(""));
    for (std::size_t n = 0; n < x.size(); n += stride) {
        double acc = 0.0;
        const std::size_t taps = std::min(h.size(), n + 1);
        for (std::size_t m = 0; m < taps; ++m)
            acc += double(h[m]) * x[n - m];
        y[n] = acc;
    }
    return y;
}

// Renders in blocks of `blockSize` (0 = a zero-length call before every 64-sample block) under the RT checker.
std::vector<float> render(PartitionedConvolver& conv, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            if (blockSize == 0)
                conv.process(in.data() + pos, out.data() + pos, 0);
            conv.process(in.data() + pos, out.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

double peakResidualDb(const std::vector<float>& y, const std::vector<double>& ref)
{
    double peak = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
        if (!std::isnan(ref[n]))
            peak = std::max(peak, std::abs(double(y[n]) - ref[n]));
    return 20.0 * std::log10(std::max(peak, 1e-30));
}

} // namespace

TEST_CASE("PartitionedConvolver: nulls against direct convolution", "[dsp][cab][conv]")
{
    // Lengths straddle the head (64) and every stage boundary (256, 1024), plus a multi-partition 1024 stage.
    const int irLength = GENERATE(1, 63, 64, 65, 255, 256, 257, 1023, 1024, 1025, 12000);
    const int blockSize = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(irLength, blockSize);

    const auto h = makeIr(irLength, 7u + static_cast<unsigned>(irLength));
    const auto x = makeNoise(irLength + 6000, 11u, 0.5f);
    const auto ref = directConvolution(x, h, irLength > 2000 ? 7 : 1); // 7 is coprime to every partition size

    PartitionedConvolver conv;
    conv.prepare(h.data(), irLength);
    REQUIRE(conv.getLatencySamples() == 0);
    const double residualDb = peakResidualDb(render(conv, x, blockSize), ref);
    CAPTURE(residualDb);
    CHECK(residualDb <= -100.0); // #69 acceptance
}

TEST_CASE("PartitionedConvolver: 1 s IR at 48 kHz nulls against direct convolution", "[dsp][cab][conv]")
{
    // The budget case of #69. The O(N·L) reference is evaluated at every 13th output (coprime to every partition
    // size), which still covers every phase of every stage.
    constexpr int kIr = 48000;
    const auto h = makeIr(kIr, 3u);
    const auto x = makeNoise(kIr + 2048, 5u, 0.5f);
    const auto ref = directConvolution(x, h, 13);

    PartitionedConvolver conv;
    conv.prepare(h.data(), kIr);
    CHECK(conv.numStages() == 3);
    const double residualDb = peakResidualDb(render(conv, x, 64), ref);
    CAPTURE(residualDb);
    CHECK(residualDb <= -100.0);
}

TEST_CASE("PartitionedConvolver: impulse in, IR out with zero latency", "[dsp][cab][conv]")
{
    const auto h = makeIr(3000, 21u);
    std::vector<float> x(4096, 0.0f);
    x[0] = 1.0f;
    PartitionedConvolver conv;
    conv.prepare(h.data(), static_cast<int>(h.size()));
    const auto y = render(conv, x, 127);
    CHECK(y[0] == h[0]); // the head is direct-form, so tap 0 is exact
    for (std::size_t n = 0; n < h.size(); ++n)
        REQUIRE(std::abs(y[n] - h[n]) < 1e-5f);
    for (std::size_t n = h.size(); n < y.size(); ++n)
        REQUIRE(std::abs(y[n]) < 1e-5f);
}

TEST_CASE("PartitionedConvolver: reset() is bit-identical, in place is allowed", "[dsp][cab][conv]")
{
    const auto h = makeIr(5000, 9u);
    const auto x = makeNoise(9000, 13u, 0.5f);
    PartitionedConvolver conv;
    conv.prepare(h.data(), static_cast<int>(h.size()));
    const auto first = render(conv, x, 127);
    conv.reset();
    const auto second = render(conv, x, 127);
    REQUIRE(first == second);

    conv.reset();
    auto inPlace = x;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < static_cast<int>(inPlace.size()); pos += 127) {
            const int n = std::min(127, static_cast<int>(inPlace.size()) - pos);
            conv.process(inPlace.data() + pos, inPlace.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    REQUIRE(inPlace == first);
}

TEST_CASE("PartitionedConvolver: before prepare() and with an empty IR it outputs silence", "[dsp][cab][conv]")
{
    const auto x = makeNoise(1000, 1u, 0.5f);
    PartitionedConvolver conv;
    REQUIRE_FALSE(conv.isPrepared());
    const auto unprepared = render(conv, x, 64);
    CHECK(std::all_of(unprepared.begin(), unprepared.end(), [](float v) { return v == 0.0f; }));

    conv.prepare(nullptr, 0);
    CHECK(conv.numStages() == 0);
    const auto empty = render(conv, x, 33);
    CHECK(std::all_of(empty.begin(), empty.end(), [](float v) { return v == 0.0f; }));
}
