// ADR-0009 D4/D7: latency rounded once from the measured fractional delays, parallel paths aligned from the same
// sums, hard-bypass null and the REQ-301 impulse test (Moth's ask on #95).

#include "osp/engine/Graph.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <utility>
#include <vector>

using osp::dsp::PartitionedConvolver;
using osp::engine::Graph;
using osp::engine::ParamStore;

namespace {

struct Rig {
    ParamStore params;
    Graph graph;
    std::vector<std::unique_ptr<PartitionedConvolver>> irs;

    Rig(double rate, double slotDelay, std::vector<std::pair<std::string, float>> settings = {})
        : graph(params, Graph::Options{slotDelay})
    {
        params.set("output.limiter", 0.0f); // keep the output linear for impulses
        for (const auto& [id, v] : settings)
            REQUIRE(params.set(id, v));
        graph.prepare(rate, 64);
        const float delta = 1.0f;
        for (int ch = 0; ch < Graph::kNumChannels; ++ch) {
            irs.push_back(std::make_unique<PartitionedConvolver>());
            irs.back()->prepare(&delta, 1);
            graph.setCabIr(ch, 0, irs.back().get());
        }
        graph.reset();
    }

    std::vector<float> render(const std::vector<float>& in)
    {
        std::vector<float> l(in.size()), r(in.size());
        const int total = static_cast<int>(in.size());
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += 64) {
                const auto p = static_cast<std::size_t>(pos);
                graph.process(in.data() + p, l.data() + p, r.data() + p, std::min(64, total - pos));
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(l);
        CHECK(l == r); // centre-panned mono path
        return l;
    }
};

// The 1 kHz group delay of an impulse response, in samples (as in the DriveRegion test).
double groupDelay1k(const std::vector<float>& h, double fs)
{
    std::complex<double> num, den;
    const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const auto e = std::polar(static_cast<double>(h[n]), -w * static_cast<double>(n));
        num += static_cast<double>(n) * e;
        den += e;
    }
    return (num / den).real();
}

std::vector<float> impulse(std::size_t n, float amplitude = 0.5f)
{
    std::vector<float> x(n, 0.0f);
    x[0] = amplitude;
    return x;
}

std::vector<float> noise(std::size_t n)
{
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> dist(-0.25f, 0.25f);
    std::vector<float> x(n);
    for (auto& v : x)
        v = dist(rng);
    return x;
}

} // namespace

TEST_CASE("Graph: hard-bypassed graph nulls against the input delayed by L_total (REQ-TEST-004)",
          "[engine][graph][latency]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double slot = GENERATE(0.0, 2.37, 2.6);
    const float blend = GENERATE(0.0f, 50.0f);
    CAPTURE(fs, slot, blend);
    Rig rig(fs, slot, {{"amp.cleanBlendPct", blend}, {"gate.enabled", 1.0f}, {"comp.enabled", 1.0f}});
    rig.graph.setBypassedAll(true);
    rig.graph.reset();
    const auto in = noise(8192);
    const auto out = rig.render(in);
    const auto null = osp::measure::nullTest(out, in, static_cast<std::size_t>(rig.graph.getLatencySamples()));
    CHECK(null.rmsDbfs <= -120.0);
}

TEST_CASE("Graph: reported latency = round(Σ D) and equals the measured 1 kHz delay within ±0.5 (REQ-301)",
          "[engine][graph][latency]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double slot = GENERATE(0.0, 2.37, 2.6);
    CAPTURE(fs, slot);
    Rig rig(fs, slot, {{"input.enabled", 0.0f}});
    const double d = groupDelay1k(rig.render(impulse(4096)), fs);
    CHECK(std::abs(d - rig.graph.getMeasuredDelaySamples()) < 0.02);
    CHECK(std::abs(d - rig.graph.getLatencySamples()) <= 0.5);
    CHECK(rig.graph.getLatencySamples() == std::lround(rig.graph.getMeasuredDelaySamples()));
}

TEST_CASE("Graph: REQ-301 end to end, Live 44.1/48 kHz, both OS-1 drives enabled (#95, Moth's ask (b))",
          "[engine][graph][latency]")
{
    // The drives run at mix 0, so the path is linear and their tone filters do not enter the 1 kHz delay; the
    // slot delay of 2.4 samples stands in for OS-2 (Phase 5). Rounding per block would be off by more than 0.5.
    const double fs = GENERATE(44100.0, 48000.0);
    Rig rig(fs, 2.4,
            {{"input.enabled", 0.0f},
             {"drive1.enabled", 1.0f},
             {"drive1.mixPct", 0.0f},
             {"drive2.enabled", 1.0f},
             {"drive2.mixPct", 0.0f}});
    const double d = groupDelay1k(rig.render(impulse(4096)), fs);
    osp::dsp::DriveRegion region;
    region.prepare(fs, 64);
    const int perBlock = region.getLatencySamples() + static_cast<int>(std::lround(2.4));
    std::printf("REQ-301 @ %5.0f Hz: measured %.3f, reported %d (per-block sum would be %d)\n", fs, d,
                rig.graph.getLatencySamples(), perBlock);
    CHECK(std::abs(d - rig.graph.getLatencySamples()) <= 0.5);
}

TEST_CASE("Graph: clean blend 100 % skips amp, cab and EQ and lands on L_total (REQ-424)", "[engine][graph][latency]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const double slot = GENERATE(0.0, 2.37);
    CAPTURE(fs, slot);
    const std::vector<std::pair<std::string, float>> clean{{"amp.cleanBlendPct", 100.0f}, {"input.enabled", 0.0f}};
    auto coloured = clean;
    coloured.insert(
        coloured.end(),
        {{"amp.ch1.levelDb", -12.0f}, {"cab.ch1.ir1.pan", 60.0f}, {"eq.enabled", 1.0f}, {"eq.ch1.g1kDb", 9.0f}});
    Rig a(fs, slot, clean), b(fs, slot, coloured);
    const auto in = noise(8192);
    const auto outA = a.render(in);
    // At 100 % the strip settings make no difference at all.
    CHECK(outA == b.render(in));
    a.graph.reset();
    const double d = groupDelay1k(a.render(impulse(4096)), fs);
    CHECK(std::abs(d - a.graph.getLatencySamples()) <= 0.5);
    CHECK(a.graph.getCleanDelaySamples() ==
          a.graph.getLatencySamples() - static_cast<int>(std::lround(a.graph.getMeasuredDelaySamples() - slot)));
}
