// osp-bench entries: reverb. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Reverb.h"
#include "osp/dsp/reverb/DattorroTank.h"
#include "osp/dsp/reverb/FdnTank.h"
#include "osp/dsp/reverb/ShimmerTank.h"
#include "osp/dsp/reverb/SpringTank.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // FDN reverb tank, hall voicing at its defaults (2.5 s, 20 % modulation): stereo, the mono input on both sides.
OSP_BENCH(fdn_hall)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::FdnTank fdn;
    fdn.prepare(kSampleRate, osp::dsp::FdnTank::kHall);
    fdn.setDecaySeconds(2.5f);
    fdn.setModulation(0.2f);
    fdn.reset();
    std::vector<float> fdnRight(kBlock);
    results.push_back({"fdn_hall", measure(bench, "fdn_hall", input, [&](const float* in, float* out, int n) {
                           fdn.process(in, in, out, fdnRight.data(), n);
                       })});
}

    // Dattorro plate tank at 2.5 s, 20 % modulation: stereo out, the mono input on both sides.
OSP_BENCH(plate_tank)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::DattorroTank plate;
    plate.prepare(kSampleRate);
    plate.setDecaySeconds(2.5f);
    plate.setModulation(0.2f);
    plate.reset();
    std::vector<float> plateRight(kBlock);
    results.push_back({"plate_tank", measure(bench, "plate_tank", input, [&](const float* in, float* out, int n) {
                           plate.process(in, in, out, plateRight.data(), n);
                       })});
}

    // Spring tank at 2.5 s, 20 % modulation: two springs (stereo out), the mono input on both sides.
OSP_BENCH(spring_tank)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::SpringTank spring;
    spring.prepare(kSampleRate);
    spring.setDecaySeconds(2.5f);
    spring.setModulation(0.2f);
    spring.reset();
    std::vector<float> springRight(kBlock);
    results.push_back({"spring_tank", measure(bench, "spring_tank", input, [&](const float* in, float* out, int n) {
                           spring.process(in, in, out, springRight.data(), n);
                       })});
}

    // Shimmer tank at 2.5 s, 20 % modulation, 40 % shimmer an octave up: the hall FDN plus the regeneration loop.
OSP_BENCH(shimmer_tank)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::ShimmerTank shimmer;
    shimmer.prepare(kSampleRate);
    shimmer.setDecaySeconds(2.5f);
    shimmer.setModulation(0.2f);
    shimmer.setShimmer(0.4f);
    shimmer.reset();
    std::vector<float> shimmerRight(kBlock);
    results.push_back({"shimmer_tank", measure(bench, "shimmer_tank", input, [&](const float* in, float* out, int n) {
                           shimmer.process(in, in, out, shimmerRight.data(), n);
                       })});
}

    // Reverb block at its defaults (hall, 2.5 s, 20 ms pre-delay, 100 Hz / 6 kHz cuts, 20 % mix): stereo in/out.
OSP_BENCH(reverb_block)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Reverb reverb;
    reverb.prepare(kSampleRate, kBlock);
    reverb.setEnabled(true);
    reverb.reset();
    std::vector<float> reverbRight(kBlock);
    results.push_back({"reverb_block", measure(bench, "reverb_block", input, [&](const float* in, float* out, int n) {
                           reverb.setParameters({});
                           reverb.process(in, in, out, reverbRight.data(), n);
                       })});
}

    // Reverb block in shimmer mode (40 % shimmer, octUp), otherwise at its defaults: the heaviest mode.
OSP_BENCH(reverb_shimmer_block)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Reverb reverbShimmer;
    reverbShimmer.prepare(kSampleRate, kBlock);
    reverbShimmer.setEnabled(true);
    osp::dsp::Reverb::Params shimmerParams;
    shimmerParams.mode = osp::dsp::Reverb::Mode::Shimmer;
    shimmerParams.shimmerPct = 40.0f;
    reverbShimmer.setParameters(shimmerParams);
    reverbShimmer.reset();
    std::vector<float> reverbShimmerRight(kBlock);
    results.push_back(
        {"reverb_shimmer_block", measure(bench, "reverb_shimmer_block", input, [&](const float* in, float* out, int n) {
             reverbShimmer.setParameters(shimmerParams);
             reverbShimmer.process(in, in, out, reverbShimmerRight.data(), n);
         })});
}

} // namespace osp::bench
