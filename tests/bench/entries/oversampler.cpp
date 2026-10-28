// osp-bench entries: oversampler. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/oversampling/Oversampler.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // IIR oversampler, Live factor at 48 kHz (4x, ADR-0005 D2), identity processing: the up/down cost of one region.
OSP_BENCH(oversampler_iir_4x)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Oversampler os4;
    os4.prepare(kSampleRate, kBlock, 4);
    results.push_back(
        {"oversampler_iir_4x", measure(bench, "oversampler_iir_4x", input, [&](const float* in, float* out, int n) {
             os4.process(in, out, n, [](float*, int) {});
         })});
}

    // FIR (HQ) oversampler at 8x, identity processing: the up/down cost of one HQ region (ADR-0005 D2, A2).
OSP_BENCH(oversampler_fir_8x)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Oversampler os8fir;
    os8fir.prepare(kSampleRate, kBlock, 8, osp::dsp::OversamplingFilter::fir);
    results.push_back(
        {"oversampler_fir_8x", measure(bench, "oversampler_fir_8x", input, [&](const float* in, float* out, int n) {
             os8fir.process(in, out, n, [](float*, int) {});
         })});
}

} // namespace osp::bench
