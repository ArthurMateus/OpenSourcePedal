// osp-bench entries: modulation. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/ChorusFlanger.h"
#include "osp/dsp/Modulation.h"
#include "osp/dsp/Phaser.h"
#include "osp/dsp/Tremolo.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Modulation, chorusWide at its defaults (0.8 Hz, depth 50 %, mix 50 %, width 100 %), stereo (#70): two ring
    // writes, four LFO-modulated Catmull-Rom reads, three sin() and the mix per sample.
OSP_BENCH(mod_chorus)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::ChorusFlanger chorus;
    chorus.prepare(kSampleRate, kBlock);
    std::vector<float> chorusRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_chorus", measure(bench, "mod_chorus", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, chorusRight.begin());
                           chorus.process(out, chorusRight.data(), n);
                       })});
}

    // Modulation, vibe (the costliest allpass type: four tan() per side) at the defaults, stereo (#70 part 2): two
    // LFO sines, the photocell one-pole, 2 x 4 TPT allpass stages and their coefficients per sample.
OSP_BENCH(mod_vibe)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Phaser vibe;
    vibe.setVoicing(osp::dsp::Phaser::Voicing::vibe);
    vibe.prepare(kSampleRate, kBlock);
    std::vector<float> vibeRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_vibe", measure(bench, "mod_vibe", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, vibeRight.begin());
                           vibe.process(out, vibeRight.data(), n);
                       })});
}

    // Modulation, harmonic tremolo at the defaults, stereo (#70 part 3): two cascaded one-poles, two band gains and
    // one LFO sine per side per sample.
OSP_BENCH(mod_tremolo)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Tremolo tremolo;
    tremolo.prepare(kSampleRate, kBlock);
    std::vector<float> tremoloRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_tremolo", measure(bench, "mod_tremolo", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, tremoloRight.begin());
                           tremolo.process(out, tremoloRight.data(), n);
                       })});
}

    // The `mod` block (#70 part 4), post position (stereo), type toggled chorusWide <-> phaser every 75 blocks
    // (100 ms), so a third of the time both slots run inside the 30 ms type crossfade.
OSP_BENCH(mod_block)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Modulation modBlock;
    modBlock.prepare(kSampleRate, kBlock);
    std::vector<float> modRight(static_cast<std::size_t>(kBlock));
    int modBlocks = 0;
    results.push_back({"mod_block", measure(bench, "mod_block", input, [&](const float* in, float* out, int n) {
                           if (++modBlocks % 75 == 0)
                               modBlock.setType(modBlock.type() == osp::dsp::Modulation::Type::phaser
                                                    ? osp::dsp::Modulation::Type::chorusWide
                                                    : osp::dsp::Modulation::Type::phaser);
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, modRight.begin());
                           modBlock.processStereo(out, modRight.data(), n);
                       })});
}

} // namespace osp::bench
