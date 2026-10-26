#include "osp/dsp/Cab.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Cab;
using osp::dsp::PartitionedConvolver;

namespace {

constexpr double kFs = 48000.0;
constexpr int kMaxBlock = 2048;

std::vector<float> makeNoise(int length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> x(static_cast<std::size_t>(length));
    for (auto& v : x)
        v = dist(rng);
    return x;
}

std::unique_ptr<PartitionedConvolver> makeConvolver(const std::vector<float>& ir)
{
    auto c = std::make_unique<PartitionedConvolver>();
    c->prepare(ir.data(), static_cast<int>(ir.size()));
    return c;
}

struct Stereo {
    std::vector<float> l, r;
};

// Renders `in` in blocks under the RT checker. `beforeBlock(pos)` runs inside the audio-thread scope, so setIr()
// and the setters are checked too. popRetired() is drained after every call into `retired`.
template <typename F>
Stereo render(
    Cab& cab, const std::vector<float>& in, int blockSize, std::vector<PartitionedConvolver*>& retired, F&& beforeBlock)
{
    Stereo out{std::vector<float>(in.size(), -1.0f), std::vector<float>(in.size(), -1.0f)};
    retired.reserve(16);
    const int total = static_cast<int>(in.size());
    const auto drain = [&] {
        while (auto* r = cab.popRetired())
            retired.push_back(r);
    };
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            beforeBlock(pos);
            drain();
            const int n = std::min(blockSize, total - pos);
            cab.process(in.data() + pos, out.l.data() + pos, out.r.data() + pos, n);
            drain();
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out.l);
    OSP_REQUIRE_OUTPUT_CLEAN(out.r);
    return out;
}

Stereo render(Cab& cab, const std::vector<float>& in, int blockSize = 64)
{
    std::vector<PartitionedConvolver*> retired;
    return render(cab, in, blockSize, retired, [](int) {});
}

std::vector<float> convolve(const std::vector<float>& ir, const std::vector<float>& x)
{
    auto c = makeConvolver(ir);
    std::vector<float> y(x.size());
    c->process(x.data(), y.data(), static_cast<int>(x.size()));
    return y;
}

} // namespace

TEST_CASE("Cab: defaults are slot 1 at unity, centred, on both channels", "[dsp][cab]")
{
    const auto ir1 = makeNoise(3000, 1u), ir2 = makeNoise(500, 2u), x = makeNoise(8000, 3u);
    auto c1 = makeConvolver(ir1), c2 = makeConvolver(ir2);
    Cab cab;
    cab.setIr(0, c1.get());
    cab.setIr(1, c2.get()); // slot 2 is off by default
    cab.prepare(kFs, kMaxBlock);
    REQUIRE(cab.getLatencySamples() == 0);
    const auto y = render(cab, x, 127);
    const auto ref = convolve(ir1, x);
    REQUIRE(y.l == ref); // exact: centre pan, 0 dB, no invert and 0 ms are all exact identities
    REQUIRE(y.r == ref);
}

TEST_CASE("Cab: level, pan law, invert and align delay", "[dsp][cab]")
{
    const std::vector<float> dirac{1.0f};
    auto c1 = makeConvolver(dirac);
    const auto x = makeNoise(4000, 4u);
    const auto settled = [](const std::vector<float>& v) { return std::vector<float>(v.begin() + 2000, v.end()); };
    const auto scaled = [&](float g, int delay) {
        std::vector<float> v(x.size(), 0.0f);
        for (std::size_t n = static_cast<std::size_t>(delay); n < x.size(); ++n)
            v[n] = g * x[n - static_cast<std::size_t>(delay)];
        return v;
    };
    const auto near = [](const std::vector<float>& a, const std::vector<float>& b, float tol) {
        for (std::size_t n = 0; n < a.size(); ++n)
            if (std::abs(a[n] - b[n]) > tol)
                return false;
        return true;
    };

    Cab cab;
    cab.setIr(0, c1.get());
    cab.prepare(kFs, kMaxBlock);

    SECTION("level -6 dB and invert")
    {
        cab.setLevelDb(0, -6.0f);
        cab.setInvert(0, true);
        const auto y = render(cab, x);
        const float g = -std::pow(10.0f, -6.0f / 20.0f);
        REQUIRE(near(settled(y.l), settled(scaled(g, 0)), 1e-6f));
        REQUIRE(near(settled(y.r), settled(scaled(g, 0)), 1e-6f));
    }
    SECTION("pan: hard left/right is +3 dB on one side, silent on the other")
    {
        const float pan = GENERATE(-100.0f, 100.0f);
        cab.setPanPct(0, pan);
        const auto y = render(cab, x);
        const auto& loud = pan < 0.0f ? y.l : y.r;
        const auto& quiet = pan < 0.0f ? y.r : y.l;
        REQUIRE(near(settled(loud), settled(scaled(std::numbers::sqrt2_v<float>, 0)), 1e-6f));
        REQUIRE(near(settled(quiet), settled(scaled(0.0f, 0)), 0.0f));
    }
    SECTION("pan: constant power at +50 %")
    {
        cab.setPanPct(0, 50.0f);
        const auto y = render(cab, x);
        const float theta = 1.5f * std::numbers::pi_v<float> / 4.0f;
        REQUIRE(near(settled(y.l), settled(scaled(std::numbers::sqrt2_v<float> * std::cos(theta), 0)), 1e-6f));
        REQUIRE(near(settled(y.r), settled(scaled(std::numbers::sqrt2_v<float> * std::sin(theta), 0)), 1e-6f));
    }
    SECTION("align: 1 ms is exactly 48 samples at 48 kHz")
    {
        cab.setDelayMs(0, 1.0f);
        const auto y = render(cab, x);
        REQUIRE(settled(y.l) == settled(scaled(1.0f, 48)));
    }
    SECTION("align: fractional delay of a low tone matches the analytic delay")
    {
        std::vector<float> tone(x.size());
        for (std::size_t n = 0; n < tone.size(); ++n)
            tone[n] =
                0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 500.0 * static_cast<double>(n) / kFs));
        const float ms = GENERATE(0.0105f, 0.5104f, 1.7f); // 0.504, 24.5 and 81.6 samples
        cab.setDelayMs(0, ms);
        const auto y = render(cab, tone);
        const double d = ms * 1e-3 * kFs;
        for (std::size_t n = 2000; n < tone.size(); ++n) {
            const double ref = 0.5 * std::sin(2.0 * std::numbers::pi * 500.0 * (static_cast<double>(n) - d) / kFs);
            REQUIRE(std::abs(y.l[n] - ref) < 2e-3); // linear below 1 sample, Catmull-Rom above
        }
    }
}

TEST_CASE("Cab: IR swap crossfades over 25 ms and retires the old object", "[dsp][cab]")
{
    const auto irA = makeNoise(2000, 5u), irB = makeNoise(2500, 6u), x = makeNoise(12000, 7u);
    auto a = makeConvolver(irA), b = makeConvolver(irB);
    Cab cab;
    cab.setIr(0, a.get());
    cab.prepare(kFs, kMaxBlock);
    constexpr int kSwapAt = 4096;
    std::vector<PartitionedConvolver*> retired;
    const auto y = render(cab, x, 64, retired, [&](int pos) {
        if (pos == kSwapAt)
            cab.setIr(0, b.get());
    });

    const auto yA = convolve(irA, x);
    const std::vector<float> tail(x.begin() + kSwapAt, x.end());
    const auto yB = convolve(irB, tail); // the incoming IR starts with an empty history at the swap
    const int fade = static_cast<int>(std::lround(Cab::kSwapSeconds * kFs));
    for (int n = 0; n < kSwapAt; ++n)
        REQUIRE(y.l[static_cast<std::size_t>(n)] == yA[static_cast<std::size_t>(n)]);
    for (int i = 0; i < fade; ++i) { // linear crossfade, the gains sum to 1
        const float g = static_cast<float>(i + 1) / static_cast<float>(fade);
        const float ref = (1.0f - g) * yA[static_cast<std::size_t>(kSwapAt + i)] + g * yB[static_cast<std::size_t>(i)];
        REQUIRE(std::abs(y.l[static_cast<std::size_t>(kSwapAt + i)] - ref) < 1e-5f); // outputs are O(10)
    }
    // The incoming convolver runs at a staggered frame phase, so it matches the phase-0 reference to rounding only.
    for (std::size_t n = static_cast<std::size_t>(kSwapAt + fade); n < x.size(); ++n)
        REQUIRE(std::abs(y.l[n] - yB[n - kSwapAt]) < 1e-5f);
    REQUIRE(retired == std::vector<PartitionedConvolver*>{a.get()});
    REQUIRE_FALSE(cab.isSwapping(0));
}

TEST_CASE("Cab: swaps during a fade queue the newest IR and retire the skipped one", "[dsp][cab]")
{
    const auto ir = makeNoise(300, 8u), x = makeNoise(9000, 9u);
    auto a = makeConvolver(ir), b = makeConvolver(ir), c = makeConvolver(ir), d = makeConvolver(ir);
    Cab cab;
    cab.setIr(0, a.get());
    cab.prepare(kFs, kMaxBlock);
    std::vector<PartitionedConvolver*> retired;
    render(cab, x, 64, retired, [&](int pos) {
        if (pos == 1024)
            cab.setIr(0, b.get());
        if (pos == 1088)
            cab.setIr(0, c.get()); // pending
        if (pos == 1152)
            cab.setIr(0, d.get()); // supersedes c, which was never heard
    });
    REQUIRE(retired == std::vector<PartitionedConvolver*>{c.get(), a.get(), b.get()});
    cab.retireAll();
    REQUIRE(cab.popRetired() == d.get());
    REQUIRE(cab.popRetired() == nullptr);
}

TEST_CASE("Cab: block bypass, hard bypass, slot 2, reset and pre-prepare", "[dsp][cab]")
{
    const auto ir1 = makeNoise(700, 10u), ir2 = makeNoise(900, 11u), x = makeNoise(6000, 12u);
    auto c1 = makeConvolver(ir1), c2 = makeConvolver(ir2);

    Cab unprepared;
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    unprepared.process(x.data(), l.data(), r.data(), 64);
    REQUIRE(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(r.begin(), r.end(), [](float v) { return v == 0.0f; }));

    Cab cab;
    cab.setIr(0, c1.get());
    cab.setIr(1, c2.get());
    cab.prepare(kFs, kMaxBlock);

    SECTION("disabled: both outputs are an exact copy of the input once the 10 ms fade is done")
    {
        cab.setEnabled(false);
        const auto y = render(cab, x);
        REQUIRE(std::equal(x.begin() + 480, x.end(), y.l.begin() + 480));
        REQUIRE(std::equal(x.begin() + 480, x.end(), y.r.begin() + 480));
        cab.setBypassed(true);
        const auto hard = render(cab, x);
        REQUIRE(hard.l == x);
        REQUIRE(hard.r == x);
    }
    SECTION("slot 2 on, panned apart, sums on top of slot 1; reset() is bit-identical")
    {
        cab.setSlotEnabled(1, true);
        cab.setPanPct(0, -100.0f);
        cab.setPanPct(1, 100.0f);
        cab.reset(); // settles every smoother at its target
        const auto y = render(cab, x, 127);
        const auto ref1 = convolve(ir1, x), ref2 = convolve(ir2, x);
        for (std::size_t n = 0; n < x.size(); ++n) {
            REQUIRE(std::abs(y.l[n] - std::numbers::sqrt2_v<float> * ref1[n]) < 1e-6f);
            REQUIRE(std::abs(y.r[n] - std::numbers::sqrt2_v<float> * ref2[n]) < 1e-6f);
        }
        cab.reset();
        const auto again = render(cab, x, 127);
        REQUIRE(again.l == y.l);
        REQUIRE(again.r == y.r);
    }
}
