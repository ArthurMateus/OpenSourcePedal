// osp-bench entries: crossfade. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Block-enable crossfade between dry and a 64-sample latency-padded copy, toggled every 75 blocks.
OSP_BENCH(crossfade_delay)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Crossfader xf;
    xf.prepare(kSampleRate);
    osp::dsp::DelayLine pad;
    pad.prepare(kBlock);
    pad.setDelay(kBlock);
    std::vector<float> padded(kBlock);
    int blockCount = 0;
    results.push_back(
        {"crossfade_delay", measure(bench, "crossfade_delay", input, [&](const float* in, float* out, int n) {
             xf.setTarget((blockCount++ / 75) % 2 == 0);
             pad.process(in, padded.data(), n);
             xf.process(in, padded.data(), out, n);
         })});
}

} // namespace osp::bench
