// osp-bench entries: the classic amp models (#174, ADR-0011 D6: one classic amp at 48 kHz / 64, Live, target <= 6 %
// of one core). Self-registering (see bench_common.h), so adding them touches no shared file.

#include "bench_common.h"

#include "../../../engine/src/amp/ClassicAmpModel.h" // the concrete type: setParameters per block, as the channel does

namespace osp::bench {

namespace {

// One model at its parameters.md defaults, Live (OS-2 4x IIR at 48 kHz), parameters set once per block as
// ModelAmpChannel will (#175).
void benchClassic(Context& ctx, const char* name, osp::engine::ClassicAmpId id, float gain)
{
    osp::engine::ClassicAmpModel amp(id);
    osp::engine::AmpParams params;
    params.gain = gain;
    amp.setParameters(params);
    amp.prepare(kSampleRate, kBlock, osp::dsp::OversamplingQuality::live);
    ctx.results.push_back({name, measure(ctx.bench, name, ctx.input, [&](const float* in, float* out, int n) {
                               amp.setParameters(params);
                               amp.process(in, out, n);
                           })});
}

} // namespace

OSP_BENCH(amp_classic_clean_a)
{
    benchClassic(ctx, "amp_classic_clean_a", osp::engine::ClassicAmpId::cleanA, 3.0f);
}

OSP_BENCH(amp_classic_crunch_a)
{
    benchClassic(ctx, "amp_classic_crunch_a", osp::engine::ClassicAmpId::crunchA, 5.0f);
}

OSP_BENCH(amp_classic_lead_a)
{
    benchClassic(ctx, "amp_classic_lead_a", osp::engine::ClassicAmpId::leadA, 6.0f);
}

} // namespace osp::bench
