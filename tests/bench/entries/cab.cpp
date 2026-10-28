// osp-bench entries: cab. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Cab.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace osp::bench {

    // Cab (#69): both slots on with 1 s IRs (the cap and the budget case), staggered 1024 frames. The mean row is the
    // steady state; the peak row is the costliest callback (see peakCallback).
OSP_BENCH(cab_2x1s)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    std::vector<std::vector<float>> cabIrs(2, std::vector<float>(static_cast<std::size_t>(kSampleRate)));
    std::uint32_t irSeed = 69u;
    for (auto& ir : cabIrs)
        for (std::size_t i = 0; i < ir.size(); ++i) {
            irSeed = irSeed * 1664525u + 1013904223u;
            const float decay = std::exp(-6.9f * static_cast<float>(i) / static_cast<float>(ir.size())); // -60 dB
            ir[i] = decay * (static_cast<float>(irSeed >> 8) / 8388608.0f - 1.0f) * 0.05f;
        }
    osp::dsp::PartitionedConvolver cabConv1, cabConv2;
    cabConv1.prepare(cabIrs[0].data(), static_cast<int>(cabIrs[0].size()));
    cabConv2.prepare(cabIrs[1].data(), static_cast<int>(cabIrs[1].size()));
    osp::dsp::Cab cab;
    cab.setIr(0, &cabConv1);
    cab.setIr(1, &cabConv2);
    cab.setSlotEnabled(1, true);
    cab.setPanPct(0, -50.0f);
    cab.setPanPct(1, 50.0f);
    cab.prepare(kSampleRate, kBlock);
    std::vector<float> cabRight(kBlock);
    const auto cabBlock = [&](const float* in, float* out, int n) { cab.process(in, out, cabRight.data(), n); };
    results.push_back({"cab_2x1s", measure(bench, "cab_2x1s", input, cabBlock)});
    results.push_back({"cab_2x1s_peak", peakCallback(input, osp::dsp::PartitionedConvolver::kMaxPartition, cabBlock)});
}

} // namespace osp::bench
