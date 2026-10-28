// osp-bench entries: tuner_tap. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/tuner/TunerTap.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Tuner tap (#75) while the tuner is open: 4th-order low pass + decimation + ring hand-off on the input, and the
    // output mute pass (unmuted, so it returns at once). The YIN detector runs on a worker and is not in this budget.
OSP_BENCH(tuner_tap)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::TunerTap tuner;
    tuner.prepare(kSampleRate, kBlock);
    results.push_back({"tuner_tap", measure(bench, "tuner_tap", input, [&](const float* in, float* out, int n) {
                           tuner.push(in, n);
                           std::copy(in, in + n, out);
                           tuner.applyMute(out, n);
                       })});
}

} // namespace osp::bench
