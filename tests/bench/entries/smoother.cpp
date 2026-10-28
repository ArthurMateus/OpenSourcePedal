// osp-bench entries: smoother. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/util/Smoothers.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

// One-pole smoother driving a gain, retargeted every 75 blocks (the per-sample cost of a "filter / tone" smoother).
OSP_BENCH(smoother_onepole)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::OnePoleSmoother pole;
    pole.prepare(kSampleRate, 0.020);
    pole.reset(1.0f);
    int blockCount = 0;
    results.push_back(
        {"smoother_onepole", measure(bench, "smoother_onepole", input, [&](const float* in, float* out, int n) {
             pole.setTarget((blockCount++ / 75) % 2 == 0 ? 0.5f : 1.0f);
             for (int i = 0; i < n; ++i)
                 out[i] = in[i] * pole.next();
         })});
}

} // namespace osp::bench
