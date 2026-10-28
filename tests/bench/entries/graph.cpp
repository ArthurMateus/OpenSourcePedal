// Full graph benchmarks (#95, ADR-0009). The two results share the original setup and object lifetimes.

#include "bench_common.h"

#include "osp/dsp/Cab.h"
#include "osp/engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace osp::bench {

OSP_BENCH(graph)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    // Same deterministic 1 s IRs as the cab ceiling benchmark in the former monolithic registry.
    std::vector<std::vector<float>> cabIrs(2, std::vector<float>(static_cast<std::size_t>(kSampleRate)));
    std::uint32_t irSeed = 69u;
    for (auto& ir : cabIrs)
        for (std::size_t i = 0; i < ir.size(); ++i) {
            irSeed = irSeed * 1664525u + 1013904223u;
            const float decay = std::exp(-6.9f * static_cast<float>(i) / static_cast<float>(ir.size())); // -60 dB
            ir[i] = decay * (static_cast<float>(irSeed >> 8) / 8388608.0f - 1.0f) * 0.05f;
        }

    // The v1 signal graph through the Engine (#95, ADR-0009): mono in, stereo out. graph_default is the default
    // preset (most blocks off, no cab IR loaded: a unit impulse); graph_all_blocks has every block on, the delay in
    // tape mode (2x saturation in the loop) and both cab slots on 1 s IRs: the full-chain ceiling case.
    std::vector<float> graphRight(kBlock);
    const auto engineBlock = [&graphRight](osp::engine::Engine& e) {
        return [&e, &graphRight](const float* in, float* out, int n) {
            std::copy_n(in, n, out);
            float* channels[] = {out, graphRight.data()};
            e.process(channels, 2, n);
        };
    };
    osp::engine::Engine graphDefault;
    graphDefault.prepare(kSampleRate, kBlock);
    results.push_back({"graph_default", measure(bench, "graph_default", input, engineBlock(graphDefault))});
    osp::dsp::PartitionedConvolver graphConv1, graphConv2; // declared first: they outlive the engine
    graphConv1.prepare(cabIrs[0].data(), static_cast<int>(cabIrs[0].size()));
    graphConv2.prepare(cabIrs[1].data(), static_cast<int>(cabIrs[1].size()));
    osp::engine::Engine graphAll;
    for (const char* id : {"gate.enabled", "comp.enabled", "drive1.enabled", "drive2.enabled", "eq.enabled",
                           "mod.enabled", "delay.enabled", "reverb.enabled", "doubler.enabled", "cab.ch1.ir2.enabled"})
        graphAll.parameters().set(id, 1.0f);
    graphAll.parameters().set("delay.mode", 2.0f); // tape
    graphAll.prepare(kSampleRate, kBlock);
    graphAll.graph().setCabIr(0, 0, &graphConv1);
    graphAll.graph().setCabIr(0, 1, &graphConv2);
    graphAll.reset(); // finishes the IR fades; the engine's own unit IR comes back through popRetired
    while (graphAll.graph().popRetired() != nullptr) {
    }
    results.push_back({"graph_all_blocks", measure(bench, "graph_all_blocks", input, engineBlock(graphAll))});
}

} // namespace osp::bench
