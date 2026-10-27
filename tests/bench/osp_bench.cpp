// osp-bench: CPU benchmarks normalised by a reference kernel (ADR-0007 §5, REQ-TEST-007).
//
// Every benchmark processes 1 s of audio at 48 kHz in 64-sample blocks. T_ref is the median time of a fixed cascade
// of 64 RBJ low-pass biquads over the same audio. For each benchmark the tool reports
//   CPU % = 100 * T_median / 1 s   and   R = T_median / T_ref,
// prints a table and writes them as JSON (--out). tools/bench/check_budget.py compares that JSON with
// tests/bench/budget.json.

#include "osp/dsp/Cab.h"
#include "osp/dsp/ChorusFlanger.h"
#include "osp/dsp/Compressor.h"
#include "osp/dsp/Doubler.h"
#include "osp/dsp/Drive.h"
#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/Eq.h"
#include "osp/dsp/Gain.h"
#include "osp/dsp/Gate.h"
#include "osp/dsp/InputStage.h"
#include "osp/dsp/Modulation.h"
#include "osp/dsp/OutputStage.h"
#include "osp/dsp/Phaser.h"
#include "osp/dsp/Reverb.h"
#include "osp/dsp/Tremolo.h"
#include "osp/dsp/delay/DelayVoice.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "osp/dsp/reverb/DattorroTank.h"
#include "osp/dsp/reverb/FdnTank.h"
#include "osp/dsp/tuner/TunerTap.h"
#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/engine/Chain.h"

#include <nanobench.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 64;
constexpr int kSamples = 48000; // 1 s of audio per benchmark iteration
constexpr int kEpochs = 21;
constexpr double kAudioSeconds = kSamples / kSampleRate;

// Deterministic input: LCG noise at about -12 dBFS, so no stage runs into denormals.
std::vector<float> makeInput()
{
    std::vector<float> x(kSamples);
    std::uint32_t s = 0x1234567u;
    for (auto& v : x) {
        s = s * 1664525u + 1013904223u;
        v = 0.25f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// RBJ cookbook low-pass, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void setLowPass(double fc, double q)
    {
        const double w = 2.0 * 3.14159265358979323846 * fc / kSampleRate;
        const double alpha = std::sin(w) / (2.0 * q);
        const double c = std::cos(w);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
        b1 = static_cast<float>((1.0 - c) / a0);
        b2 = b0;
        a1 = static_cast<float>(-2.0 * c / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void process(float* x, int n) noexcept
    {
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const float out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            x[i] = out;
        }
    }
};

struct Result {
    std::string name;
    double seconds;
};

// Runs `perBlock(in, out, n)` over 1 s of audio in 64-sample blocks, kEpochs times, and returns the median time.
// A template rather than std::function, so the per-block call can inline and costs nothing in the measurement (#83).
template <typename PerBlock>
double measure(ankerl::nanobench::Bench& bench, const char* name, const std::vector<float>& input, PerBlock&& perBlock)
{
    std::vector<float> out(input.size());
    bench.run(name, [&] {
        for (int pos = 0; pos < kSamples; pos += kBlock)
            perBlock(input.data() + pos, out.data() + pos, kBlock);
        ankerl::nanobench::doNotOptimizeAway(out[static_cast<std::size_t>(kSamples - 1)]);
    });
    return bench.results().back().median(ankerl::nanobench::Result::Measure::elapsed);
}

// Costliest callback of a block whose work repeats every `period` samples (the cab's 1024 stage): every block is timed
// over kPeakCycles periods, the median is taken per block phase (robust to preemption), and the worst phase wins.
// Returned as the time for 1 s of audio if every block cost that much, so CPU % reads as the peak share of a block.
double peakCallback(const std::vector<float>& input,
                    int period,
                    const std::function<void(const float*, float*, int)>& perBlock)
{
    constexpr int kPeakCycles = 200;
    const int phases = period / kBlock;
    std::vector<std::vector<double>> byPhase(static_cast<std::size_t>(phases));
    std::vector<float> out(kBlock);
    const int blocksPerInput = kSamples / kBlock;
    for (int b = 0; b < (kPeakCycles + 4) * phases; ++b) {
        const float* in = input.data() + static_cast<std::ptrdiff_t>(b % blocksPerInput) * kBlock;
        const auto t0 = std::chrono::steady_clock::now();
        perBlock(in, out.data(), kBlock);
        const std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
        if (b >= 4 * phases) // warm-up
            byPhase[static_cast<std::size_t>(b % phases)].push_back(dt.count());
    }
    double worst = 0.0;
    for (auto& v : byPhase) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        worst = std::max(worst, v[v.size() / 2]);
    }
    ankerl::nanobench::doNotOptimizeAway(out[0]);
    return worst * blocksPerInput;
}

} // namespace

int main(int argc, char** argv)
{
    const char* outPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            std::fprintf(stderr, "usage: osp-bench [--out results.json]\n");
            return 2;
        }
    }

#ifdef _WIN32
    // REQ-TEST-007 step 1: high priority and one pinned core, to cut scheduler noise on CI runners.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);
#endif

    const auto input = makeInput();
    ankerl::nanobench::Bench bench;
    bench.title("osp-bench (48 kHz, 64-sample blocks, 1 s of audio per op)").unit("s audio").epochs(kEpochs).warmup(1);

    // REQ-TEST-007 step 2: T_ref, 64 RBJ low-passes at 200 Hz ... 12.8 kHz (log-spaced), Q = 0.707.
    std::array<Biquad, 64> cascade;
    for (std::size_t k = 0; k < cascade.size(); ++k)
        cascade[k].setLowPass(200.0 * std::pow(2.0, static_cast<double>(k) * 6.0 / 63.0), 0.7071);
    const double tRef = measure(bench, "reference: 64 x RBJ biquad", input, [&](const float* in, float* out, int n) {
        std::memcpy(out, in, static_cast<std::size_t>(n) * sizeof(float));
        for (auto& bq : cascade)
            bq.process(out, n);
    });

    std::vector<Result> results;

    // Gain block, with the target toggled every 75 blocks (100 ms) so that ramps are part of the cost.
    osp::dsp::Gain gain;
    gain.prepare(kSampleRate, kBlock);
    int blockCount = 0;
    results.push_back({"gain", measure(bench, "gain", input, [&](const float* in, float* out, int n) {
                           gain.setGainDb((blockCount++ / 75) % 2 == 0 ? -6.0f : 0.0f);
                           gain.process(in, out, n);
                       })});

    // One-pole smoother driving a gain, retargeted every 75 blocks (the per-sample cost of a "filter / tone" smoother).
    osp::dsp::OnePoleSmoother pole;
    pole.prepare(kSampleRate, 0.020);
    pole.reset(1.0f);
    blockCount = 0;
    results.push_back(
        {"smoother_onepole", measure(bench, "smoother_onepole", input, [&](const float* in, float* out, int n) {
             pole.setTarget((blockCount++ / 75) % 2 == 0 ? 0.5f : 1.0f);
             for (int i = 0; i < n; ++i)
                 out[i] = in[i] * pole.next();
         })});

    // Block-enable crossfade between dry and a 64-sample latency-padded copy, toggled every 75 blocks.
    osp::dsp::Crossfader xf;
    xf.prepare(kSampleRate);
    osp::dsp::DelayLine pad;
    pad.prepare(kBlock);
    pad.setDelay(kBlock);
    std::vector<float> padded(kBlock);
    blockCount = 0;
    results.push_back(
        {"crossfade_delay", measure(bench, "crossfade_delay", input, [&](const float* in, float* out, int n) {
             xf.setTarget((blockCount++ / 75) % 2 == 0);
             pad.process(in, padded.data(), n);
             xf.process(in, padded.data(), out, n);
         })});

    // EQ, worst static case: graphic9 with every band at +-6 dB and both cuts on (mono; linked stereo is about 2x).
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

    // IIR oversampler, Live factor at 48 kHz (4x, ADR-0005 D2), identity processing: the up/down cost of one region.
    osp::dsp::Oversampler os4;
    os4.prepare(kSampleRate, kBlock, 4);
    results.push_back(
        {"oversampler_iir_4x", measure(bench, "oversampler_iir_4x", input, [&](const float* in, float* out, int n) {
             os4.process(in, out, n, [](float*, int) {});
         })});

    // One drive slot (mid, defaults) inside a 4x IIR region: OS-1 at 48 kHz with one drive enabled (ADR-0005 D2).
    osp::dsp::Oversampler osDrive;
    osDrive.prepare(kSampleRate, kBlock, 4);
    osp::dsp::DriveStage drive;
    drive.prepare(kSampleRate * 4, kBlock * 4);
    results.push_back(
        {"drive_stage_4x", measure(bench, "drive_stage_4x", input, [&](const float* in, float* out, int n) {
             osDrive.process(in, out, n, [&](float* x, int osN) { drive.process(x, osN); });
         })});

    // OS-1 with both slots enabled (mid into dist), Live at 48 kHz: the full drive section of a stacked preset.
    osp::dsp::DriveRegion os1;
    os1.slot(0).setEnabled(true);
    os1.slot(1).setEnabled(true);
    os1.slot(1).setType(osp::dsp::DriveType::dist);
    os1.prepare(kSampleRate, kBlock);
    results.push_back(
        {"drive_region_live", measure(bench, "drive_region_live", input,
                                      [&](const float* in, float* out, int n) { os1.process(in, out, n); })});

    // Noise gate (enabled, defaults) on the noise input: peak detector + state machine + gain per sample.
    osp::dsp::Gate gate;
    gate.prepare(kSampleRate, kBlock);
    gate.setEnabled(true);
    gate.reset();
    results.push_back(
        {"gate", measure(bench, "gate", input, [&](const float* in, float* out, int n) { gate.process(in, out, n); })});

    // Tuner tap (#75) while the tuner is open: 4th-order low pass + decimation + ring hand-off on the input, and the
    // output mute pass (unmuted, so it returns at once). The YIN detector runs on a worker and is not in this budget.
    osp::dsp::TunerTap tuner;
    tuner.prepare(kSampleRate, kBlock);
    results.push_back({"tuner_tap", measure(bench, "tuner_tap", input, [&](const float* in, float* out, int n) {
                           tuner.push(in, n);
                           std::copy(in, in + n, out);
                           tuner.applyMute(out, n);
                       })});

    // Input block (#74) at +6 dB trim: constant-gain trim + one-pole/one-zero DC blocker per sample.
    osp::dsp::InputStage inputStage;
    inputStage.prepare(kSampleRate, kBlock);
    inputStage.setGainDb(6.0f);
    inputStage.reset();
    results.push_back({"input_stage", measure(bench, "input_stage", input, [&](const float* in, float* out, int n) {
                           inputStage.process(in, out, n);
                       })});

    // Output block (#74), stereo, limiter on and engaged (+12 dB level into the -0.3 dBTP ceiling): NaN guard, level,
    // 4x interpolated true-peak detector (3 phases x 12 taps x 2 channels), gain and meters per sample.
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

    // Doubler (#74) at its defaults (50 %, width 100 %, 18 ms, 8 cents), stereo: two ring writes, two LFO-modulated
    // Catmull-Rom reads and the pan/mix per sample.
    osp::dsp::Doubler doubler;
    doubler.prepare(kSampleRate, kBlock);
    std::vector<float> dblRight(static_cast<std::size_t>(kBlock));
    results.push_back({"doubler", measure(bench, "doubler", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, dblRight.begin());
                           doubler.process(out, dblRight.data(), n);
                       })});

    // Modulation, chorusWide at its defaults (0.8 Hz, depth 50 %, mix 50 %, width 100 %), stereo (#70): two ring
    // writes, four LFO-modulated Catmull-Rom reads, three sin() and the mix per sample.
    osp::dsp::ChorusFlanger chorus;
    chorus.prepare(kSampleRate, kBlock);
    std::vector<float> chorusRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_chorus", measure(bench, "mod_chorus", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, chorusRight.begin());
                           chorus.process(out, chorusRight.data(), n);
                       })});

    // Modulation, vibe (the costliest allpass type: four tan() per side) at the defaults, stereo (#70 part 2): two
    // LFO sines, the photocell one-pole, 2 x 4 TPT allpass stages and their coefficients per sample.
    osp::dsp::Phaser vibe;
    vibe.setVoicing(osp::dsp::Phaser::Voicing::vibe);
    vibe.prepare(kSampleRate, kBlock);
    std::vector<float> vibeRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_vibe", measure(bench, "mod_vibe", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, vibeRight.begin());
                           vibe.process(out, vibeRight.data(), n);
                       })});

    // Modulation, harmonic tremolo at the defaults, stereo (#70 part 3): two cascaded one-poles, two band gains and
    // one LFO sine per side per sample.
    osp::dsp::Tremolo tremolo;
    tremolo.prepare(kSampleRate, kBlock);
    std::vector<float> tremoloRight(static_cast<std::size_t>(kBlock));
    results.push_back({"mod_tremolo", measure(bench, "mod_tremolo", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, tremoloRight.begin());
                           tremolo.process(out, tremoloRight.data(), n);
                       })});

    // The `mod` block (#70 part 4), post position (stereo), type toggled chorusWide <-> phaser every 75 blocks
    // (100 ms), so a third of the time both slots run inside the 30 ms type crossfade.
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

    // Compressor at its defaults (opto, -20 dB, 4:1, all three detectors run) with the tone tilt active (knob 7).
    osp::dsp::Compressor comp;
    comp.setTone(7.0f);
    comp.prepare(kSampleRate, kBlock);
    results.push_back({"comp", measure(bench, "comp", input, [&](const float* in, float* out, int n) {
                           comp.setThresholdDb(-20.0f);
                           comp.process(in, out, n);
                       })});

    // Delay voice, digital, ping-pong at the defaults (400 ms, feedback 30 %, mix 25 %, cuts 80 Hz / 5 kHz), stereo
    // (#71): two Catmull-Rom reads, four TPT one-poles, two ring writes and the mix per sample.
    osp::dsp::DelayVoice delayDigital;
    delayDigital.setStereo(osp::dsp::DelayVoice::Stereo::pingpong);
    delayDigital.prepare(kSampleRate, kBlock);
    std::vector<float> delayRight(static_cast<std::size_t>(kBlock));
    results.push_back({"delay_digital", measure(bench, "delay_digital", input, [&](const float* in, float* out, int n) {
                           std::copy(in, in + n, out);
                           std::copy(in, in + n, delayRight.begin());
                           delayDigital.process(out, delayRight.data(), n);
                       })});

    // FDN reverb tank, hall voicing at its defaults (2.5 s, 20 % modulation): stereo, the mono input on both sides.
    osp::dsp::FdnTank fdn;
    fdn.prepare(kSampleRate, osp::dsp::FdnTank::kHall);
    fdn.setDecaySeconds(2.5f);
    fdn.setModulation(0.2f);
    fdn.reset();
    std::vector<float> fdnRight(kBlock);
    results.push_back({"fdn_hall", measure(bench, "fdn_hall", input, [&](const float* in, float* out, int n) {
                           fdn.process(in, in, out, fdnRight.data(), n);
                       })});

    // Dattorro plate tank at 2.5 s, 20 % modulation: stereo out, the mono input on both sides.
    osp::dsp::DattorroTank plate;
    plate.prepare(kSampleRate);
    plate.setDecaySeconds(2.5f);
    plate.setModulation(0.2f);
    plate.reset();
    std::vector<float> plateRight(kBlock);
    results.push_back({"plate_tank", measure(bench, "plate_tank", input, [&](const float* in, float* out, int n) {
                           plate.process(in, in, out, plateRight.data(), n);
                       })});

    // Reverb block at its defaults (hall, 2.5 s, 20 ms pre-delay, 100 Hz / 6 kHz cuts, 20 % mix): stereo in/out.
    osp::dsp::Reverb reverb;
    reverb.prepare(kSampleRate, kBlock);
    reverb.setEnabled(true);
    reverb.reset();
    std::vector<float> reverbRight(kBlock);
    results.push_back({"reverb_block", measure(bench, "reverb_block", input, [&](const float* in, float* out, int n) {
                           reverb.setParameters({});
                           reverb.process(in, in, out, reverbRight.data(), n);
                       })});
    // Cab (#69): both slots on with 1 s IRs (the cap and the budget case), staggered 1024 frames. The mean row is the
    // steady state; the peak row is the costliest callback (see peakCallback).
    std::vector<std::vector<float>> cabIrs(2, std::vector<float>(static_cast<std::size_t>(kSampleRate)));
    std::uint32_t irSeed = 69u;
    for (auto& ir : cabIrs)
        for (std::size_t i = 0; i < ir.size(); ++i) {
            irSeed = irSeed * 1664525u + 1013904223u;
            const float decay = std::exp(-6.9f * static_cast<float>(i) / static_cast<float>(ir.size())); // -60 dB
            ir[i] = decay * (static_cast<float>(irSeed >> 8) / 8388608.0f - 1.0f) * 0.05f;
        }
    osp::dsp::PartitionedConvolver cabConv1, cabConv2;
    cabConv1.prepare(cabIrs[0].data(), static_cast<int>(cabIrs[0].size()));
    cabConv2.prepare(cabIrs[1].data(), static_cast<int>(cabIrs[1].size()));
    osp::dsp::Cab cab;
    cab.setIr(0, &cabConv1);
    cab.setIr(1, &cabConv2);
    cab.setSlotEnabled(1, true);
    cab.setPanPct(0, -50.0f);
    cab.setPanPct(1, 50.0f);
    cab.prepare(kSampleRate, kBlock);
    std::vector<float> cabRight(kBlock);
    const auto cabBlock = [&](const float* in, float* out, int n) { cab.process(in, out, cabRight.data(), n); };
    results.push_back({"cab_2x1s", measure(bench, "cab_2x1s", input, cabBlock)});
    results.push_back({"cab_2x1s_peak", peakCallback(input, osp::dsp::PartitionedConvolver::kMaxPartition, cabBlock)});
    // FIR (HQ) oversampler at 8x, identity processing: the up/down cost of one HQ region (ADR-0005 D2, A2).
    osp::dsp::Oversampler os8fir;
    os8fir.prepare(kSampleRate, kBlock, 8, osp::dsp::OversamplingFilter::fir);
    results.push_back(
        {"oversampler_fir_8x", measure(bench, "oversampler_fir_8x", input, [&](const float* in, float* out, int n) {
             os8fir.process(in, out, n, [](float*, int) {});
         })});

    // Pass-through chain (the full engine chain as it stands), at 0 dB.
    osp::engine::Chain chain;
    chain.prepare(kSampleRate, kBlock);
    results.push_back(
        {"passthrough_chain", measure(bench, "passthrough_chain", input, [&](const float* in, float* out, int n) {
             chain.setOutputLevelDb(0.0f);
             chain.process(in, out, n);
         })});

    std::printf("\nT_ref = %.6f s per 1 s of audio\n", tRef);
    std::printf("%-20s %12s %10s %10s\n", "benchmark", "T_median s", "CPU %", "R");
    for (const auto& r : results)
        std::printf("%-20s %12.3e %10.4f %10.6f\n", r.name.c_str(), r.seconds, 100.0 * r.seconds / kAudioSeconds,
                    r.seconds / tRef);

    if (outPath != nullptr) {
        std::FILE* f = std::fopen(outPath, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "osp-bench: cannot write %s\n", outPath);
            return 1;
        }
        std::fprintf(f, "{\n  \"schemaVersion\": 1,\n  \"sampleRate\": %.0f,\n  \"blockSize\": %d,\n", kSampleRate,
                     kBlock);
        std::fprintf(f, "  \"epochs\": %d,\n  \"tRefSeconds\": %.9e,\n  \"benchmarks\": {\n", kEpochs, tRef);
        for (std::size_t i = 0; i < results.size(); ++i)
            std::fprintf(f, "    \"%s\": { \"tMedianSeconds\": %.9e, \"cpuPercent\": %.6f, \"r\": %.9f }%s\n",
                         results[i].name.c_str(), results[i].seconds, 100.0 * results[i].seconds / kAudioSeconds,
                         results[i].seconds / tRef, i + 1 < results.size() ? "," : "");
        std::fprintf(f, "  }\n}\n");
        std::fclose(f);
        std::printf("wrote %s\n", outPath);
    }
    return 0;
}
