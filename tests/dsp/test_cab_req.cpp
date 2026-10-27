// Cab block (#69 part 4): REQ-TEST-001 matrix, REQ-TEST-003 smoothing residuals for every smoothed parameter and the
// IR swap, REQ-TEST-004 hard-bypass null, and the RT fuzz with out-of-range / NaN / Inf parameters.

#include "osp/dsp/Cab.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::Cab;
using osp::dsp::PartitionedConvolver;

namespace {

constexpr int kMaxBlock = 2048;

std::unique_ptr<PartitionedConvolver> makeConvolver(const std::vector<float>& ir)
{
    auto c = std::make_unique<PartitionedConvolver>();
    c->prepare(ir.data(), static_cast<int>(ir.size()));
    return c;
}

std::vector<float> noise(int n, unsigned seed, float amplitude)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-amplitude, amplitude);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}

struct Stereo {
    std::vector<float> l, r;
};

// Renders in blocks of `block` (0: a zero-length call before every 64-sample block) under the RT checker;
// `beforeBlock(pos)` runs inside the audio-thread scope.
Stereo render(Cab& cab, const std::vector<float>& in, int block, const std::function<void(int)>& beforeBlock = {})
{
    Stereo y{std::vector<float>(in.size()), std::vector<float>(in.size())};
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < static_cast<int>(in.size()); pos += step) {
            if (beforeBlock)
                beforeBlock(pos);
            const int n = std::min(step, static_cast<int>(in.size()) - pos);
            if (block == 0)
                cab.process(in.data() + pos, y.l.data() + pos, y.r.data() + pos, 0);
            cab.process(in.data() + pos, y.l.data() + pos, y.r.data() + pos, n);
            while (cab.popRetired() != nullptr) {
            }
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(y.l);
    OSP_REQUIRE_OUTPUT_CLEAN(y.r);
    return y;
}

// Linear law from `from` to `to` over `len` samples, starting at sample `at` (LinearRamp: the first step lands at at).
double ramp(int i, int at, int len, double from, double to)
{
    return i < at ? from : from + (to - from) * std::min(1.0, static_cast<double>(i - at + 1) / len);
}

void panGains(double p, double& gl, double& gr)
{
    const double theta = (p + 1.0) * std::numbers::pi / 4.0;
    gl = std::numbers::sqrt2 * std::cos(theta);
    gr = std::numbers::sqrt2 * std::sin(theta);
}

} // namespace

TEST_CASE("Cab: every smoothed parameter and the IR swap leave <= -60 dBc residual (REQ-TEST-003)",
          "[dsp][cab][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const int block = GENERATE(37, 64, 512);
    const bool up = GENERATE(true, false);
    CAPTURE(fs, block, up);
    const auto tone = osp::measure::coherentTone(1000.0, fs);
    const int n = static_cast<int>(0.4 * fs);
    const auto x = osp::measure::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const int at = (n / 2 + block - 1) / block * block; // first sample of the block after the change
    const auto ms = [&](double t) { return static_cast<int>(std::lround(t * 1e-3 * fs)); };
    const std::vector<float> unit{1.0f}, quarter{0.25f}, half{0.5f};
    std::vector<double> refL(static_cast<std::size_t>(n)), refR(static_cast<std::size_t>(n));

    // Runs one step: `setup` before prepare-time settling, `change` at `at`, `law(i, gl, gr)` the float64 reference.
    auto check = [&](const char* what, const std::function<void(Cab&)>& setup, const std::function<void(Cab&)>& change,
                     const std::function<void(int, double&, double&)>& law) {
        auto a = makeConvolver(unit), b = makeConvolver(half);
        Cab cab;
        cab.setIr(0, a.get());
        cab.setIr(1, b.get()); // slot 2 (off unless a case enables it): a 0.5 dirac
        setup(cab);
        cab.prepare(fs, kMaxBlock);
        const auto y = render(cab, x, block, [&](int pos) {
            if (pos == at)
                change(cab);
        });
        for (int i = 0; i < n; ++i) {
            double gl = 0.0, gr = 0.0;
            law(i, gl, gr);
            refL[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)] * gl;
            refR[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)] * gr;
        }
        const double rl = osp::measure::residualDbc(y.l, refL), rr = osp::measure::residualDbc(y.r, refR);
        CAPTURE(what, rl, rr);
        WARN("REQ-TEST-003 cab " << what << " fs " << fs << " block " << block << (up ? " up" : " down") << ": L " << rl
                                 << " dBc, R " << rr << " dBc");
        CHECK(rl <= -60.0);
        CHECK(rr <= -60.0);
    };

    const double g0 = up ? 1.0 : std::pow(10.0, -1.0), g1 = up ? std::pow(10.0, -1.0) : 1.0;
    check(
        "level 0 <-> -20 dB", [&](Cab& cab) { cab.setLevelDb(0, up ? 0.0f : -20.0f); },
        [&](Cab& cab) { cab.setLevelDb(0, up ? -20.0f : 0.0f); },
        [&](int i, double& gl, double& gr) { gl = gr = ramp(i, at, ms(20), g0, g1); });
    check(
        "pan 0 <-> +100 %", [&](Cab& cab) { cab.setPanPct(0, up ? 0.0f : 100.0f); },
        [&](Cab& cab) { cab.setPanPct(0, up ? 100.0f : 0.0f); },
        [&](int i, double& gl, double& gr) { panGains(ramp(i, at, ms(20), up ? 0.0 : 1.0, up ? 1.0 : 0.0), gl, gr); });
    check(
        "invert", [&](Cab& cab) { cab.setInvert(0, !up); }, [&](Cab& cab) { cab.setInvert(0, up); },
        [&](int i, double& gl, double& gr) { gl = gr = ramp(i, at, ms(10), up ? 1.0 : -1.0, up ? -1.0 : 1.0); });
    check(
        "slot 2 enable", [&](Cab& cab) { cab.setSlotEnabled(1, !up); }, [&](Cab& cab) { cab.setSlotEnabled(1, up); },
        [&](int i, double& gl, double& gr) {
            gl = gr = 1.0 + 0.5 * ramp(i, at, ms(10), up ? 0.0 : 1.0, up ? 1.0 : 0.0);
        });
    check(
        "block enable (wet -6 dB)",
        [&](Cab& cab) {
            cab.setLevelDb(0, -6.0f);
            cab.setEnabled(!up);
        },
        [&](Cab& cab) { cab.setEnabled(up); },
        [&](int i, double& gl, double& gr) {
            const double p = ramp(i, at, ms(10), up ? 0.0 : 1.0, up ? 1.0 : 0.0);
            gl = gr = (1.0 - p) + p * std::pow(10.0, -6.0 / 20.0);
        });

    // IR swap: dirac 1 -> 0.25 over the ADR-0004 swap time (the new IR starts cold, which a 1-tap IR does not see).
    {
        auto a = makeConvolver(unit), c = makeConvolver(quarter);
        Cab cab;
        cab.setIr(0, (up ? a : c).get());
        cab.prepare(fs, kMaxBlock);
        const auto y = render(cab, x, block, [&](int pos) {
            if (pos == at)
                cab.setIr(0, (up ? c : a).get());
        });
        const int len = static_cast<int>(std::lround(Cab::kSwapSeconds * fs));
        for (int i = 0; i < n; ++i) {
            const double g = ramp(i, at, len, 0.0, 1.0);
            refL[static_cast<std::size_t>(i)] =
                x[static_cast<std::size_t>(i)] * (up ? (1.0 - g) + 0.25 * g : 0.25 * (1.0 - g) + g);
        }
        const double r = osp::measure::residualDbc(y.l, refL);
        WARN("REQ-TEST-003 cab IR swap fs " << fs << " block " << block << ": " << r << " dBc");
        CHECK(r <= -60.0);
    }

    // Align 0 <-> 1 ms: a linear 20 ms ramp of the delay, reference = the tone at the time-varying delay.
    {
        auto a = makeConvolver(unit);
        Cab cab;
        cab.setIr(0, a.get());
        cab.setDelayMs(0, up ? 0.0f : 1.0f);
        cab.prepare(fs, kMaxBlock);
        const auto y = render(cab, x, block, [&](int pos) {
            if (pos == at)
                cab.setDelayMs(0, up ? 1.0f : 0.0f);
        });
        const double amp = std::pow(10.0, -12.0 / 20.0), dMax = 1e-3 * fs;
        for (int i = 0; i < n; ++i) {
            const double d = ramp(i, at, ms(20), up ? 0.0 : dMax, up ? dMax : 0.0);
            const double t = static_cast<double>(i) - d;
            refL[static_cast<std::size_t>(i)] =
                t < 0.0 ? 0.0 : amp * std::sin(2.0 * std::numbers::pi * tone.frequency * t / fs);
        }
        const double r = osp::measure::residualDbc(y.l, refL);
        WARN("REQ-TEST-003 cab align fs " << fs << " block " << block << (up ? " up" : " down") << ": " << r << " dBc");
        CHECK(r <= -60.0);
    }
}

TEST_CASE("Cab: rate x block-size matrix is bit-identical to a 64-sample render (REQ-TEST-001)", "[dsp][cab]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    const auto ir1 = noise(3000, 1u, 0.3f), ir2 = noise(2000, 2u, 0.3f);
    const auto x = noise(static_cast<int>(0.3 * fs), 3u, 0.5f);
    auto run = [&](int block) {
        auto a = makeConvolver(ir1), b = makeConvolver(ir2);
        Cab cab;
        cab.setIr(0, a.get());
        cab.setIr(1, b.get());
        cab.setSlotEnabled(1, true);
        cab.setLevelDb(0, -3.0f);
        cab.setLevelDb(1, -9.0f);
        cab.setPanPct(0, -40.0f);
        cab.setPanPct(1, 70.0f);
        cab.setInvert(1, true);
        cab.setDelayMs(1, 0.37f); // fractional at every rate
        cab.prepare(fs, kMaxBlock);
        return render(cab, x, block);
    };
    const auto ref = run(64);
    for (int block : {0, 1, 32, 127, 512, 2048}) {
        CAPTURE(block);
        const auto y = run(block);
        REQUIRE(y.l == ref.l);
        REQUIRE(y.r == ref.r);
    }
}

TEST_CASE("Cab: hard bypass nulls against the input on both channels (REQ-TEST-004)", "[dsp][cab]")
{
    const auto ir = noise(500, 4u, 0.3f);
    const auto x = noise(48000, 5u, 0.9f);
    auto c = makeConvolver(ir);
    Cab cab;
    cab.setIr(0, c.get());
    cab.prepare(48000.0, kMaxBlock);
    cab.setBypassed(true);
    const auto y = render(cab, x, 127);
    const auto nl = osp::measure::nullTest(y.l, x), nr = osp::measure::nullTest(y.r, x);
    CAPTURE(nl.peakDbfs, nr.peakDbfs);
    REQUIRE(nl.peakDbfs <= -120.0);
    REQUIRE(nr.peakDbfs <= -120.0);
}

TEST_CASE("Cab: 1000 random parameter vectors (out of range, NaN, Inf), random swaps, subnormal input",
          "[dsp][cab][rt]")
{
    const double fs = 48000.0;
    std::vector<std::unique_ptr<PartitionedConvolver>> pool;
    for (unsigned k = 0; k < 6; ++k)
        pool.push_back(makeConvolver(noise(4000 + 3000 * static_cast<int>(k), 10u + k, 0.2f)));
    std::vector<PartitionedConvolver*> spare;
    for (std::size_t k = 2; k < pool.size(); ++k)
        spare.push_back(pool[k].get());
    spare.reserve(pool.size() + Cab::kRetireCapacity);

    Cab cab;
    cab.setIr(0, pool[0].get());
    cab.setIr(1, pool[1].get());
    cab.prepare(fs, kMaxBlock);

    std::mt19937 rng(69u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float specials[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()};
    auto value = [&](float lo, float hi) { // 10 % specials, else 25 % beyond the range on either side
        if (u(rng) < 0.1f)
            return specials[static_cast<std::size_t>(u(rng) * 2.999f)];
        const float span = hi - lo;
        return lo - 0.25f * span + 1.5f * span * u(rng);
    };
    auto in = noise(kMaxBlock, 6u, 1.0f);
    for (std::size_t i = 0; i < in.size(); i += 7)
        in[i] = (i % 2 ? 1.0f : -1.0f) * 1e-40f; // subnormal samples (DAZ treats them as 0)
    std::vector<float> l(kMaxBlock), r(kMaxBlock);
    std::size_t bad = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            for (int s = 0; s < Cab::kNumSlots; ++s) {
                cab.setSlotEnabled(s, u(rng) < 0.7f);
                cab.setLevelDb(s, value(-60.0f, 6.0f));
                cab.setPanPct(s, value(-100.0f, 100.0f));
                cab.setInvert(s, u(rng) < 0.5f);
                cab.setDelayMs(s, value(0.0f, 2.0f));
            }
            cab.setEnabled(u(rng) < 0.9f);
            if (u(rng) < 0.05f && !spare.empty()) { // a new IR into a random slot
                cab.setIr(u(rng) < 0.5f ? 0 : 1, spare.back());
                spare.pop_back();
            }
            const int n = 1 + static_cast<int>(u(rng) * (kMaxBlock - 1));
            cab.process(in.data(), l.data(), r.data(), n);
            while (auto* ir = cab.popRetired()) {
                ir->reset(); // cold again, so it can be installed once more
                spare.push_back(ir);
            }
            // Non-finite never; subnormal only as the input sample itself, passed bit-exactly by the disabled block's
            // dry copy (REQ-TEST-004: bypass is an exact copy; DAZ flushes every arithmetic use of it).
            for (int i = 0; i < n; ++i)
                for (const float y : {l[static_cast<std::size_t>(i)], r[static_cast<std::size_t>(i)]})
                    if (!std::isfinite(y) ||
                        (std::fpclassify(y) == FP_SUBNORMAL && y != in[static_cast<std::size_t>(i)]))
                        ++bad;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    REQUIRE(bad == 0u);

    // Then silence: once the longest IR (19 000 taps) and every fade have passed, the output is exactly 0.
    cab.setEnabled(true);
    const std::vector<float> silence(static_cast<std::size_t>(1.0 * fs), 0.0f);
    const auto tail = render(cab, silence, 512);
    REQUIRE(std::all_of(tail.l.end() - 512, tail.l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(tail.r.end() - 512, tail.r.end(), [](float v) { return v == 0.0f; }));
}
