// osp-bench entries: compressor. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Compressor.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Compressor at its defaults (opto, -20 dB, 4:1, all three detectors run) with the tone tilt active (knob 7).
OSP_BENCH(comp)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Compressor comp;
    comp.setTone(7.0f);
    comp.prepare(kSampleRate, kBlock);
    results.push_back({"comp", measure(bench, "comp", input, [&](const float* in, float* out, int n) {
                           comp.setThresholdDb(-20.0f);
                           comp.process(in, out, n);
                       })});
}

} // namespace osp::bench
