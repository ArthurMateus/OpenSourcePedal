// RtResampler (ADR-0002 D4): the fork of AudioDSPTools' ResamplingContainer around a 48 kHz inner callable.
// Latency and level per REQ-NAM-004, error counter per REQ-NAM-002, RT checker per realtime-rules.md C1-C4/C8.

#include "../../engine/src/nam/RtResampler.h" // private engine header (engine/src is not a public include dir)

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

using osp::engine::nam::RtResampler;

namespace {

constexpr double kModelRate = 48000.0;

// The NAM-shaped callable: capture-free identity at the inner rate.
void identity(const float* in, float* out, int n) noexcept
{
    std::copy(in, in + n, out);
}

// Runs `x` through `r` in the given block sizes as the audio thread, and checks the RT checker and the output.
std::vector<float> render(RtResampler& r, const std::vector<float>& x, const std::vector<int>& blocks)
{
    std::vector<float> y(x.size(), 1.0f);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        std::size_t pos = 0;
        for (std::size_t b = 0; pos < x.size(); b = (b + 1) % blocks.size()) {
            const auto n = std::min(static_cast<std::size_t>(blocks[b]), x.size() - pos);
            r.process(x.data() + pos, y.data() + pos, static_cast<int>(n), identity);
            pos += n;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(y);
    return y;
}

// Delay of the impulse response peak, refined by parabolic interpolation (samples, fractional).
double peakDelay(const std::vector<float>& y, std::size_t impulseAt)
{
    std::size_t k = impulseAt;
    for (std::size_t i = impulseAt; i < y.size(); ++i)
        if (std::fabs(y[i]) > std::fabs(y[k]))
            k = i;
    const double a = y[k - 1], b = y[k], c = y[k + 1];
    const double den = a - 2.0 * b + c;
    return static_cast<double>(k - impulseAt) + (den != 0.0 ? 0.5 * (a - c) / den : 0.0);
}

struct Tone {
    double amplitude, phase;
};

// Amplitude and phase at `hz` by quadrature correlation over `len` samples (a whole number of cycles).
Tone fitTone(const std::vector<float>& y, std::size_t start, std::size_t len, double hz, double fs)
{
    double s = 0.0, c = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
        const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(start + i) / fs;
        s += y[start + i] * std::sin(w);
        c += y[start + i] * std::cos(w);
    }
    return {2.0 * std::hypot(s, c) / static_cast<double>(len), std::atan2(c, s)};
}

} // namespace

TEST_CASE("RtResampler: reported latency equals the measured impulse delay +-1 sample", "[engine][nam][resampler]")
{
    const double fs = GENERATE(44100.0, 88200.0, 96000.0);
    const int block = GENERATE(1, 64, 127, 2048);
    const std::size_t impulseAt = GENERATE(std::size_t{300}, std::size_t{301}, std::size_t{437});
    CAPTURE(fs, block, impulseAt);

    RtResampler r;
    REQUIRE(r.prepare(fs, kModelRate, block));
    const int latency = r.getLatencySamples();
    CHECK(latency > 0);
    CHECK(latency < 100);

    std::vector<float> x(4096, 0.0f);
    x[impulseAt] = 1.0f;
    const auto y = render(r, x, {block});
    const double measured = peakDelay(y, impulseAt);
    CAPTURE(latency, measured);
    CHECK(std::fabs(measured - latency) <= 1.0);
    CHECK(r.errorCount() == 0u);
}

TEST_CASE("RtResampler: 1 kHz sine level within +-0.5 dB, phase delay matches the latency", "[engine][nam][resampler]")
{
    const double fs = GENERATE(44100.0, 88200.0, 96000.0);
    CAPTURE(fs);
    RtResampler r;
    REQUIRE(r.prepare(fs, kModelRate, 64));

    const auto total = static_cast<std::size_t>(fs); // 1 s
    std::vector<float> x(total);
    for (std::size_t i = 0; i < total; ++i)
        x[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / fs));
    const auto y = render(r, x, {64});

    const auto len = static_cast<std::size_t>(fs / 10.0); // 100 whole cycles
    const std::size_t start = total / 2;
    const Tone in = fitTone(x, start, len, 1000.0, fs);
    const Tone out = fitTone(y, start, len, 1000.0, fs);
    const double levelDb = 20.0 * std::log10(out.amplitude / in.amplitude);
    // Phase delay in samples, unwrapped to the period nearest the reported latency.
    const double period = fs / 1000.0;
    double delay = (in.phase - out.phase) / (2.0 * std::numbers::pi) * period;
    delay += period * std::round((r.getLatencySamples() - delay) / period);
    CAPTURE(levelDb, delay, r.getLatencySamples(), r.getDelaySamples());
    CHECK(std::fabs(levelDb) <= 0.5);
    CHECK(std::fabs(delay - r.getLatencySamples()) <= 1.0);
    CHECK(std::fabs(delay - r.getDelaySamples()) <= 0.01); // the exact (fractional) delay
    CHECK(r.errorCount() == 0u);
}

TEST_CASE("RtResampler: random block sizes 0..maxBlock: error counter 0, RT clean, block-size invariant",
          "[engine][nam][resampler][rt]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int maxBlock = GENERATE(1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, maxBlock);

    std::mt19937 rng(static_cast<unsigned>(maxBlock * 7 + static_cast<int>(fs)));
    std::uniform_int_distribution<int> size(0, maxBlock);
    std::vector<int> blocks(257);
    for (auto& b : blocks)
        b = size(rng);
    blocks.push_back(maxBlock); // at least one non-zero block, so the loop advances
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> x(static_cast<std::size_t>(fs / 4.0));
    for (auto& v : x)
        v = noise(rng);

    RtResampler a;
    REQUIRE(a.prepare(fs, kModelRate, maxBlock));
    const auto yRandom = render(a, x, blocks);
    CHECK(a.errorCount() == 0u);

    RtResampler b;
    REQUIRE(b.prepare(fs, kModelRate, maxBlock));
    const auto yFixed = render(b, x, {maxBlock});
    CHECK(b.errorCount() == 0u);
    CHECK(yRandom == yFixed); // rational phases: the output does not depend on how the host splits blocks

    // reset() restores the prepared state: a second render is bit-identical.
    const auto rt = osp::rt::test::runAsAudioThread([&] { b.reset(); });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(render(b, x, {maxBlock}) == yFixed);
}

TEST_CASE("RtResampler: the inner callable never gets more than getMaxInnerBlockSize()", "[engine][nam][resampler]")
{
    const double fs = GENERATE(44100.0, 88200.0, 96000.0);
    const int maxBlock = GENERATE(1, 127, 2048);
    RtResampler r;
    REQUIRE(r.prepare(fs, kModelRate, maxBlock));
    int largest = 0;
    std::vector<float> x(8192, 0.25f), y(x.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos + static_cast<std::size_t>(maxBlock) <= x.size(); pos += maxBlock)
            r.process(x.data() + pos, y.data() + pos, maxBlock, [&largest](const float* in, float* out, int n) {
                largest = std::max(largest, n);
                std::copy(in, in + n, out);
            });
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(largest >= 1);
    CHECK(largest <= r.getMaxInnerBlockSize());
    CHECK(r.errorCount() == 0u);
}

TEST_CASE("RtResampler: contract violations zero-fill; only an oversized block counts an error",
          "[engine][nam][resampler]")
{
    std::vector<float> x(256, 0.5f), y(256, 1.0f);
    RtResampler r;
    r.process(x.data(), y.data(), 64, identity); // before prepare(): silence, not an error
    CHECK(std::all_of(y.begin(), y.begin() + 64, [](float v) { return v == 0.0f; }));
    CHECK(r.errorCount() == 0u);

    CHECK_FALSE(r.prepare(0.0, kModelRate, 64));
    CHECK_FALSE(r.prepare(44100.0, kModelRate, 0));
    REQUIRE(r.prepare(44100.0, kModelRate, 64));
    std::fill(y.begin(), y.end(), 1.0f);
    r.process(x.data(), y.data(), 0, identity);
    CHECK(y[0] == 1.0f); // n = 0 touches nothing
    r.process(x.data(), y.data(), 65, identity);
    CHECK(std::all_of(y.begin(), y.begin() + 65, [](float v) { return v == 0.0f; }));
    CHECK(r.errorCount() == 1u);
}
