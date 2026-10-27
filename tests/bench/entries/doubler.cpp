// osp-bench entries: doubler. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Doubler.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Doubler (#74) at its defaults (50 %, width 100 %, 18 ms, 8 cents), stereo: two ring writes, two LFO-modulated
    // Catmull-Rom reads and the pan/mix per sample.
OSP_BENCH(doubler)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Doubler doubler;
    doubler.prepare(kSampleRate, kBlock);
    std::vector<float> dblRight(static_cast<std::size_t>(kBlock));
    results.push_back({"doubler", measure(bench, "doubler", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, dblRight.begin());
                           doubler.process(out, dblRight.data(), n);
                       })});
}

} // namespace osp::bench
