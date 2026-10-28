// osp-bench entries: input_stage. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/InputStage.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Input block (#74) at +6 dB trim: constant-gain trim + one-pole/one-zero DC blocker per sample.
OSP_BENCH(input_stage)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::InputStage inputStage;
    inputStage.prepare(kSampleRate, kBlock);
    inputStage.setGainDb(6.0f);
    inputStage.reset();
    // Keep this unit kernel specialised when graph.cpp adds more callers under MSVC LTCG (#165).
    // The full graph benchmarks retain the production Engine call path.
    results.push_back({"input_stage", measure(bench, "input_stage", input, [&](const float* in, float* out, int n) {
#if defined(_MSC_VER)
                           [[msvc::forceinline_calls]]
#endif
                           inputStage.process(in, out, n);
                       })});
}

} // namespace osp::bench
