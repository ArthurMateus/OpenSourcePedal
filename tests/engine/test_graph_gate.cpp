// gate.postAmp (ADR-0009 D6): the post-amp gain lands exactly K_gate samples after the key, and moving the tap
// under signal is level-neutral through the linear amp stub (REQ-TEST-003 residual limit).

#include "osp/engine/Graph.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using osp::dsp::PartitionedConvolver;
using osp::engine::Graph;
using osp::engine::ParamStore;

namespace {

constexpr int kBlock = 64;

struct Rig {
    ParamStore params;
    Graph graph;
    std::vector<std::unique_ptr<PartitionedConvolver>> irs;

    Rig(double rate, double slotDelay, const std::vector<std::pair<std::string, float>>& settings)
        : graph(params, Graph::Options{slotDelay})
    {
        params.set("input.enabled", 0.0f); // no DC blocker: the gate key is exactly the input
        params.set("output.limiter", 0.0f);
        for (const auto& [id, v] : settings)
            REQUIRE(params.set(id, v));
        graph.prepare(rate, kBlock);
        const float delta = 1.0f;
        for (int ch = 0; ch < Graph::kNumChannels; ++ch) {
            irs.push_back(std::make_unique<PartitionedConvolver>());
            irs.back()->prepare(&delta, 1);
            graph.setCabIr(ch, 0, irs.back().get());
        }
        graph.reset();
    }

    std::vector<float> render(const std::vector<float>& in, const std::function<void(int)>& beforeBlock = {})
    {
        std::vector<float> l(in.size()), r(in.size());
        const int total = static_cast<int>(in.size());
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += kBlock) {
                if (beforeBlock)
                    beforeBlock(pos);
                const auto p = static_cast<std::size_t>(pos);
                graph.process(in.data() + p, l.data() + p, r.data() + p, std::min(kBlock, total - pos));
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(l);
        return l;
    }
};

// Silence, an impulse key at n0, then a 1 kHz burst: the gate opens on the impulse with its attack.
std::vector<float> keyed(double fs, std::size_t n, std::size_t n0)
{
    std::vector<float> x(n, 0.0f);
    x[n0] = 0.5f;
    for (std::size_t i = n0 + 1; i < n; ++i)
        x[i] = 0.25f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / fs));
    return x;
}

} // namespace

TEST_CASE("Graph: gate.postAmp gain edge lands exactly K_gate samples after the key (±0)", "[engine][graph][gate]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double slot = GENERATE(0.0, 2.37);
    CAPTURE(fs, slot);
    const std::vector<std::pair<std::string, float>> common{{"gate.thresholdDb", -40.0f}, {"gate.attackMs", 0.1f}};
    auto post = common;
    post.insert(post.end(), {{"gate.enabled", 1.0f}, {"gate.postAmp", 1.0f}});
    Rig open(fs, slot, common), gated(fs, slot, post);
    const auto in = keyed(fs, 4096, 1000);
    const auto y = open.render(in); // gate off: the linear chain
    const auto out = gated.render(in);

    // The reference gain: the same gate on the same key, in the same blocks.
    osp::dsp::Gate gate;
    gate.prepare(fs, kBlock);
    osp::dsp::Gate::Params gp;
    gp.thresholdDb = -40.0f;
    gp.attackMs = 0.1f;
    gate.setParameters(gp);
    gate.reset();
    std::vector<float> g(in.size());
    for (std::size_t pos = 0; pos < in.size(); pos += kBlock)
        gate.computeGain(in.data() + pos, g.data() + pos, kBlock);

    const int k = gated.graph.getGateTapDelaySamples();
    CHECK(k == std::lround(gated.graph.getMeasuredDelaySamples()));
    const auto error = [&](int shift) {
        double worst = 0.0;
        for (std::size_t n = 0; n < in.size(); ++n) {
            const long src = static_cast<long>(n) - shift;
            // The graph's arithmetic: h = 1 − (1 − g), carried as the attenuation 1 − h; 1 before the start.
            const float h = src >= 0 ? 1.0f - (1.0f - (1.0f - (1.0f - g[static_cast<std::size_t>(src)]))) : 1.0f;
            worst = std::max(worst, std::abs(static_cast<double>(out[n]) - static_cast<double>(y[n]) * h));
        }
        return worst;
    };
    const double atK = error(k), early = error(k - 1), late = error(k + 1);
    std::printf("gate.postAmp @ %5.0f Hz, slot %.2f: K_gate %d, max error %.2e (K-1: %.2e, K+1: %.2e)\n", fs, slot, k,
                atK, early, late);
    CHECK(atK < 1e-5);
    CHECK(early > 100.0 * atK);
    CHECK(late > 100.0 * atK);
}

TEST_CASE("Graph: toggling gate.postAmp under signal is level-neutral (xf 10, REQ-TEST-003 <= -60 dBc)",
          "[engine][graph][gate]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const double slot = GENERATE(0.0, 2.37);
    CAPTURE(fs, slot);
    // A steady sine below the threshold: the gate sits closed at its −20 dB range floor.
    const std::vector<std::pair<std::string, float>> settings{
        {"gate.enabled", 1.0f}, {"gate.thresholdDb", -20.0f}, {"gate.rangeDb", -20.0f}};
    const auto in = osp::measure::makeTone(osp::measure::coherentTone(1000.0, fs), fs, -30.0, 65536);
    Rig still(fs, slot, settings), toggled(fs, slot, settings);
    const auto ref = still.render(in);
    const int on = 19999 / kBlock, off = 40001 / kBlock; // toggled at block boundaries, pre → post → pre
    const int tap = osp::engine::findParam("gate.postAmp");
    const auto out = toggled.render(in, [&](int pos) {
        if (pos / kBlock == on)
            toggled.params.set(tap, 1.0f);
        if (pos / kBlock == off)
            toggled.params.set(tap, 0.0f);
    });
    const double dbc = osp::measure::residualDbc(out, ref);
    std::printf("gate.postAmp toggle @ %5.0f Hz, slot %.2f: residual %.1f dBc\n", fs, slot, dbc);
    CHECK(dbc <= -60.0);
}
