// osp-bench entries: chain. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/engine/Chain.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Pass-through chain (the full engine chain as it stands), at 0 dB.
OSP_BENCH(passthrough_chain)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::engine::Chain chain;
    chain.prepare(kSampleRate, kBlock);
    results.push_back(
        {"passthrough_chain", measure(bench, "passthrough_chain", input, [&](const float* in, float* out, int n) {
             chain.setOutputLevelDb(0.0f);
             chain.process(in, out, n);
         })});
}

} // namespace osp::bench
