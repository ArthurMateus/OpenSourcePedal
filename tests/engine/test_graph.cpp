#include "osp/engine/Graph.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using osp::dsp::PartitionedConvolver;
using osp::engine::Graph;
using osp::engine::ParamStore;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double rate, double hz, float amp, int n)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / rate));
    return x;
}

struct Stereo {
    std::vector<float> l, r;
};

// A prepared graph with a unit-impulse IR in ir1 of every strip, so the cab is an identity.
struct Rig {
    ParamStore params;
    Graph graph;
    std::vector<std::unique_ptr<PartitionedConvolver>> irs;
    std::vector<PartitionedConvolver*> retired;

    Rig(double rate, int maxBlock, Graph::Options options = {}) : graph(params, options)
    {
        graph.prepare(rate, maxBlock);
        const float delta = 1.0f;
        for (int ch = 0; ch < Graph::kNumChannels; ++ch) {
            irs.push_back(std::make_unique<PartitionedConvolver>());
            irs.back()->prepare(&delta, 1);
            graph.setCabIr(ch, 0, irs.back().get());
        }
        graph.reset(); // finishes the IR fade-in
        retired.reserve(16);
    }

    void set(const std::string& id, float value) { REQUIRE(params.set(id, value)); }

    // Renders under the RT checker; `beforeBlock(pos)` runs on the audio thread before each block.
    Stereo render(const std::vector<float>& in, int block, const std::function<void(int)>& beforeBlock = {})
    {
        Stereo out{std::vector<float>(in.size()), std::vector<float>(in.size())};
        const int total = static_cast<int>(in.size());
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += block) {
                if (beforeBlock)
                    beforeBlock(pos);
                const int n = std::min(block, total - pos);
                const auto p = static_cast<std::size_t>(pos);
                graph.process(in.data() + p, out.l.data() + p, out.r.data() + p, n);
                while (auto* r = graph.popRetired())
                    retired.push_back(r);
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(out.l);
        OSP_REQUIRE_OUTPUT_CLEAN(out.r);
        return out;
    }
};

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    double sum = 0.0;
    for (std::size_t i = from; i < a.size(); ++i)
        sum += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
    return std::sqrt(sum / static_cast<double>(a.size() - from));
}

double maxStep(const std::vector<float>& x)
{
    double m = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::abs(static_cast<double>(x[i]) - x[i - 1]));
    return m;
}

} // namespace

TEST_CASE("Graph: default chain renders finite and RT-clean at every rate x block size", "[engine][graph]")
{
    const double rate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(1, 32, 127, 512, 2048);
    Rig rig(rate, 2048);
    const auto in = sine(rate, 1000.0, 0.25f, 6000);
    const auto out = rig.render(in, block);
    CHECK(rmsDiff(out.l, out.r, 0) == 0.0); // centre-panned mono IR
}

TEST_CASE("Graph: every block is reachable and parameter-addressable; disabled again it is exactly the default",
          "[engine][graph]")
{
    struct Case {
        const char* block;
        std::vector<std::pair<std::string, float>> set; // applied at 0.1 s, enabled cleared at 0.4 s
        bool exactAfterDisable; // false for the blocks whose tails ring on (delay, reverb)
    };
    const std::vector<Case> cases{
        {"input", {{"input.gainDb", 6.0f}}, true},
        {"gate", {{"gate.enabled", 1.0f}, {"gate.thresholdDb", -20.0f}}, true},
        {"comp", {{"comp.enabled", 1.0f}, {"comp.thresholdDb", -45.0f}}, true},
        {"drive1", {{"drive1.enabled", 1.0f}, {"drive1.drive", 8.0f}}, true},
        {"drive2", {{"drive2.enabled", 1.0f}, {"drive2.type", 3.0f}}, true},
        {"amp", {{"amp.ch1.levelDb", -6.0f}}, true},
        {"cab", {{"cab.ch1.ir1.levelDb", -6.0f}}, true},
        {"eq", {{"eq.enabled", 1.0f}, {"eq.ch1.g1kDb", 9.0f}}, true},
        {"mod", {{"mod.enabled", 1.0f}, {"mod.type", 5.0f}, {"mod.depthPct", 100.0f}}, true},
        {"delay", {{"delay.enabled", 1.0f}, {"delay.timeMs", 50.0f}, {"delay.mixPct", 60.0f}}, false}, // trails ring on
        {"reverb", {{"reverb.enabled", 1.0f}, {"reverb.mixPct", 60.0f}}, false}, // trails ring on
        {"doubler", {{"doubler.enabled", 1.0f}, {"doubler.amountPct", 100.0f}}, true},
        {"output", {{"output.levelDb", -6.0f}}, true},
    };
    const double rate = 48000.0;
    const auto in = sine(rate, 997.0, 0.05f, 36000); // −26 dBFS: below the gate threshold under test
    Rig base(rate, 64);
    const auto ref = base.render(in, 64);

    for (const auto& c : cases) {
        INFO(c.block);
        Rig rig(rate, 64);
        // Off again at 0.4 s: enabled = false, or the parameter back to its default. Indices are looked up here,
        // off the audio thread.
        std::vector<std::pair<int, float>> on;
        for (const auto& [id, v] : c.set)
            on.emplace_back(osp::engine::findParam(id), v);
        const int offIndex = on.front().first;
        const bool isEnable = c.set.front().first.ends_with(".enabled");
        const float offValue = isEnable ? 0.0f : osp::engine::paramTable()[static_cast<std::size_t>(offIndex)].def;
        const auto out = rig.render(in, 64, [&](int pos) {
            if (pos == 4800)
                for (const auto& [index, v] : on)
                    rig.params.set(index, v);
            if (pos == 19200)
                rig.params.set(offIndex, offValue);
        });
        // Addressable: the block changed the output while it was on.
        std::vector<float> onRef(ref.l.begin() + 9600, ref.l.begin() + 19200);
        std::vector<float> onOut(out.l.begin() + 9600, out.l.begin() + 19200);
        CHECK(rmsDiff(onRef, onOut, 0) > 1e-3);
        // Click-free: no sample step larger than the sine's own slope plus margin (2π·f/fs·A ≈ 0.013 at +9 dB).
        CHECK(maxStep(out.l) < 0.25);
        if (c.exactAfterDisable)
            CHECK(rmsDiff(ref.l, out.l, 28800) < 1e-6);
    }
}

TEST_CASE("Graph: spring, shimmer amount and every shimmer interval reach the reverb", "[engine][graph][reverb]")
{
    const double rate = GENERATE(44100.0, 48000.0, 96000.0);
    constexpr int block = 127; // odd host blocks exercise the same fan-out used by the app
    const int samples = static_cast<int>(3.0 * rate);
    auto in = sine(rate, 440.0, 0.2f, samples);
    std::fill(in.begin() + static_cast<int>(0.2 * rate), in.end(), 0.0f); // a note followed by its reverb tail
    const auto render = [&](float mode, float amount, float interval) {
        Rig rig(rate, block);
        rig.set("input.enabled", 0.0f);
        rig.set("output.limiter", 0.0f);
        rig.set("reverb.enabled", 1.0f);
        rig.set("reverb.mode", mode);
        rig.set("reverb.mixPct", 100.0f);
        rig.set("reverb.preDelayMs", 0.0f);
        rig.set("reverb.shimmerPct", amount);
        rig.set("reverb.shimmerInterval", interval);
        rig.graph.reset(); // start in the requested mode, without a transition from the default hall
        return rig.render(in, block).l;
    };
    const auto plate = render(2.0f, 0.0f, 0.0f);
    const auto spring = render(3.0f, 0.0f, 0.0f);
    const auto hall = render(1.0f, 0.0f, 0.0f);
    const auto noShimmer = render(4.0f, 0.0f, 0.0f);
    const auto octave = render(4.0f, 80.0f, 0.0f);
    const auto fifth = render(4.0f, 80.0f, 1.0f);
    const auto down = render(4.0f, 80.0f, 2.0f);
    const auto tail = static_cast<std::size_t>(0.3 * rate);
    // Each comparison fails if its selector/amount is ignored or spring/shimmer still use plate/hall fallbacks.
    CHECK(rmsDiff(spring, plate, tail) > 1e-5);
    CHECK(rmsDiff(octave, hall, tail) > 1e-5);
    CHECK(rmsDiff(octave, noShimmer, tail) > 1e-5);
    CHECK(rmsDiff(octave, fifth, tail) > 1e-5);
    CHECK(rmsDiff(octave, down, tail) > 1e-5);
    CHECK(rmsDiff(fifth, down, tail) > 1e-5);
}

TEST_CASE("Graph: amp.channel crossfades the strips over the ADR-0004 time, with no step", "[engine][graph]")
{
    const double rate = 48000.0;
    Rig rig(rate, 64);
    rig.set("amp.ch2.levelDb", -6.0f);
    rig.set("cab.ch2.ir1.levelDb", -6.0f);
    const auto in = sine(rate, 997.0, 0.25f, 24000);
    const auto out = rig.render(in, 64, [&](int pos) {
        if (pos == 9600)
            rig.params.set("amp.channel", 1.0f);
    });
    CHECK(rig.graph.activeChannel() == 1);
    const double slope = 2.0 * kPi * 997.0 / rate * 0.25;
    // From 0.1 s on: the start of the sine is a step through the OS-1 filters, not the switch at 0.2 s.
    CHECK(maxStep(std::vector<float>(out.l.begin() + 4800, out.l.end())) <= slope * 1.01);
    // Settled on ch2: −12 dB.
    double peak = 0.0;
    for (std::size_t i = 18000; i < out.l.size(); ++i)
        peak = std::max(peak, std::abs(static_cast<double>(out.l[i])));
    CHECK(std::abs(20.0 * std::log10(peak / 0.25) + 12.0) < 0.1);
}

TEST_CASE("Graph: mod.position moves the block without a step", "[engine][graph]")
{
    const double rate = 48000.0;
    Rig rig(rate, 64);
    rig.set("mod.enabled", 1.0f);
    rig.set("mod.type", 5.0f); // tremolo
    const auto in = sine(rate, 997.0, 0.25f, 24000);
    const int position = osp::engine::findParam("mod.position");
    const auto out = rig.render(in, 64, [&](int pos) {
        if (pos == 9600)
            rig.params.set(position, 1.0f);
    });
    CHECK(maxStep(std::vector<float>(out.l.begin() + 4800, out.l.end())) < 0.05);
}

TEST_CASE("Graph: latency is round(D_os1 + D_slot), with a fractional amp-slot test delay", "[engine][graph]")
{
    const double rate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    osp::dsp::DriveRegion region;
    region.prepare(rate, 64);
    for (const double slot : {0.0, 2.37, 3.6}) {
        Rig rig(rate, 64, Graph::Options{slot});
        CHECK(rig.graph.getLatencySamples() == std::lround(region.getMeasuredDelaySamples() + slot));
    }
}

TEST_CASE("Graph: installed IRs come back through popRetired, never deleted by the graph", "[engine][graph]")
{
    Rig rig(48000.0, 64);
    auto replacement = std::make_unique<PartitionedConvolver>();
    const float half = 0.5f;
    replacement->prepare(&half, 1);
    const auto in = sine(48000.0, 997.0, 0.25f, 4800);
    rig.render(in, 64, [&](int pos) {
        if (pos == 0)
            rig.graph.setCabIr(0, 0, replacement.get());
    });
    REQUIRE(rig.retired.size() == 1);
    CHECK(rig.retired[0] == rig.irs[0].get());
}
