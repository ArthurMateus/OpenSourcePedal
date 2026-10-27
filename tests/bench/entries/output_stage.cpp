// osp-bench entries: output_stage. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/OutputStage.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Output block (#74), stereo, limiter on and engaged (+12 dB level into the -0.3 dBTP ceiling): NaN guard, level,
    // 4x interpolated true-peak detector (3 phases x 12 taps x 2 channels), gain and meters per sample.
OSP_BENCH(output_stage)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::OutputStage outputStage;
    outputStage.prepare(kSampleRate, kBlock);
    outputStage.setLevelDb(12.0f);
    outputStage.reset();
    std::vector<float> outRight(static_cast<std::size_t>(kBlock));
    results.push_back({"output_stage", measure(bench, "output_stage", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, outRight.begin());
                           outputStage.process(out, outRight.data(), n);
                       })});
}

} // namespace osp::bench
