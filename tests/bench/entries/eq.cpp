// osp-bench entries: eq. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Eq.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // EQ, worst static case: graphic9 with every band at +-6 dB and both cuts on (mono; linked stereo is about 2x).
OSP_BENCH(eq)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Eq eq;
    for (int band = 0; band < 9; ++band)
        eq.setGraphic9Db(band, band % 2 == 0 ? 6.0f : -6.0f);
    eq.setLowCutHz(80.0f);
    eq.setHighCutHz(10000.0f);
    eq.prepare(kSampleRate, kBlock);
    results.push_back({"eq", measure(bench, "eq", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           float* ch[1] = {out};
                           eq.process(ch, 1, n);
                       })});
}

} // namespace osp::bench
