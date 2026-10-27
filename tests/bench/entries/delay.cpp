// osp-bench entries: delay. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Delay.h"
#include "osp/dsp/delay/DelayVoice.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // Delay voice, digital, ping-pong at the defaults (400 ms, feedback 30 %, mix 25 %, cuts 80 Hz / 5 kHz), stereo
    // (#71): two Catmull-Rom reads, four TPT one-poles, two ring writes and the mix per sample.
OSP_BENCH(delay_digital)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::DelayVoice delayDigital;
    delayDigital.setStereo(osp::dsp::DelayVoice::Stereo::pingpong);
    delayDigital.prepare(kSampleRate, kBlock);
    std::vector<float> delayRight(static_cast<std::size_t>(kBlock));
    results.push_back({"delay_digital", measure(bench, "delay_digital", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, delayRight.begin());
                           delayDigital.process(out, delayRight.data(), n);
                       })});
}

    // Delay voice, tape, ping-pong at the defaults plus modulation 50 % (#71 part 3): the digital loop plus, per side,
    // a 4.5 kHz SVF, the 2x IIR oversampler with ADAA1 tanh inside the loop, wow/flutter and the head bump.
OSP_BENCH(delay_tape)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::DelayVoice delayTape;
    delayTape.setVoicing(osp::dsp::DelayVoice::Voicing::tape);
    delayTape.setStereo(osp::dsp::DelayVoice::Stereo::pingpong);
    delayTape.setModDepthPct(50.0f);
    delayTape.prepare(kSampleRate, kBlock);
    std::vector<float> delayRight(static_cast<std::size_t>(kBlock));
    results.push_back({"delay_tape", measure(bench, "delay_tape", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, delayRight.begin());
                           delayTape.process(out, delayRight.data(), n);
                       })});
}

    // The delay block (#71 part 4): ping-pong, ducking 6 dB, toggling digital <-> tape every 100 ms, so 30 % of the
    // time both slots run inside the 30 ms mode crossfade and each toggle copies the voice state.
OSP_BENCH(delay_block)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Delay delayBlock;
    delayBlock.setStereo(osp::dsp::Delay::Stereo::pingpong);
    delayBlock.setDuckDb(6.0f);
    delayBlock.prepare(kSampleRate, kBlock);
    std::vector<float> delayRight(static_cast<std::size_t>(kBlock));
    int delayBlocks = 0;
    results.push_back({"delay_block", measure(bench, "delay_block", input, [&](const float* in, float* out, int n) {
                           if (++delayBlocks % 75 == 0) // 75 blocks of 64 at 48 kHz = 100 ms
                               delayBlock.setMode(delayBlock.mode() == osp::dsp::Delay::Mode::digital
                                                      ? osp::dsp::Delay::Mode::tape
                                                      : osp::dsp::Delay::Mode::digital);
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, delayRight.begin());
                           delayBlock.process(out, delayRight.data(), n);
                       })});
}

} // namespace osp::bench
