// osp-bench entries: gate. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Gate.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Noise gate (enabled, defaults) on the noise input: peak detector + state machine + gain per sample.
OSP_BENCH(gate)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Gate gate;
    gate.prepare(kSampleRate, kBlock);
    gate.setEnabled(true);
    gate.reset();
    results.push_back(
        {"gate", measure(bench, "gate", input, [&](const float* in, float* out, int n) { gate.process(in, out, n); })});
}

} // namespace osp::bench
