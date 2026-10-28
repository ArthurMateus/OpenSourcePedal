// osp-bench entries: gain. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Gain.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Gain block, with the target toggled every 75 blocks (100 ms) so that ramps are part of the cost.
OSP_BENCH(gain)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Gain gain;
    gain.prepare(kSampleRate, kBlock);
    int blockCount = 0;
    results.push_back({"gain", measure(bench, "gain", input, [&](const float* in, float* out, int n) {
                           gain.setGainDb((blockCount++ / 75) % 2 == 0 ? -6.0f : 0.0f);
                           gain.process(in, out, n);
                       })});
}

} // namespace osp::bench
