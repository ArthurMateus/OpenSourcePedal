// osp-bench: CPU benchmarks normalised by a reference kernel (ADR-0007 §5, REQ-TEST-007).
//
// Every benchmark processes 1 s of audio at 48 kHz in 64-sample blocks. T_ref is the median time of a fixed cascade
// of 64 RBJ low-pass biquads over the same audio. For each benchmark the tool reports
//   CPU % = 100 * T_median / 1 s   and   R = T_median / T_ref,
// prints a table and writes them as JSON (--out). tools/bench/check_budget.py compares that JSON with
// tests/bench/budget.json.

#include "osp/dsp/Gain.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/engine/Chain.h"

#include <nanobench.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <array>
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
double measure(ankerl::nanobench::Bench& bench,
               const char* name,
               const std::vector<float>& input,
               const std::function<void(const float*, float*, int)>& perBlock)
{
    std::vector<float> out(input.size());
    bench.run(name, [&] {
        for (int pos = 0; pos < kSamples; pos += kBlock)
            perBlock(input.data() + pos, out.data() + pos, kBlock);
        ankerl::nanobench::doNotOptimizeAway(out[static_cast<std::size_t>(kSamples - 1)]);
    });
    return bench.results().back().median(ankerl::nanobench::Result::Measure::elapsed);
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

    // IIR oversampler, Live factor at 48 kHz (4x, ADR-0005 D2), identity processing: the up/down cost of one region.
    osp::dsp::Oversampler os4;
    os4.prepare(kSampleRate, kBlock, 4);
    results.push_back(
        {"oversampler_iir_4x", measure(bench, "oversampler_iir_4x", input, [&](const float* in, float* out, int n) {
             os4.process(in, out, n, [](float*, int) {});
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
