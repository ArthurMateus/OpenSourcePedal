// Shared pieces of osp-bench (ADR-0007 section 5, REQ-TEST-007): constants, the measuring helpers and the registry.
//
// Each benchmark lives in tests/bench/entries/<block>.cpp and registers itself with OSP_BENCH(<name>), so adding a
// block adds a file and touches no shared list. Its budget goes in tests/bench/budget.d/<block>.json (merged by
// tools/bench/check_budget.py). osp_bench.cpp runs the entries sorted by <name>.
#pragma once

#include <nanobench.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace osp::bench {

inline constexpr double kSampleRate = 48000.0;
inline constexpr int kBlock = 64;
inline constexpr int kSamples = 48000; // 1 s of audio per benchmark iteration
inline constexpr int kEpochs = 21;
inline constexpr double kAudioSeconds = kSamples / kSampleRate;

struct Result {
    std::string name;
    double seconds;
};

// What an entry gets: the bench, the shared noise input, and the list its results are appended to.
struct Context {
    ankerl::nanobench::Bench& bench;
    const std::vector<float>& input;
    std::vector<Result>& results;
};

using EntryFn = void (*)(Context&);

struct Entry {
    const char* name;
    EntryFn fn;
};

inline std::vector<Entry>& registry()
{
    static std::vector<Entry> entries; // function-local: safe against static initialisation order
    return entries;
}

struct Registrar {
    Registrar(const char* name, EntryFn fn) { registry().push_back({name, fn}); }
};

// Defines and registers `void <id>_run(Context& ctx)`; the body follows the macro.
#define OSP_BENCH(id)                                                                                                  \
    static void id##_run(::osp::bench::Context& ctx);                                                                  \
    static const ::osp::bench::Registrar id##_registrar(#id, &id##_run);                                               \
    static void id##_run(::osp::bench::Context& ctx)

// Runs `perBlock(in, out, n)` over 1 s of audio in 64-sample blocks, kEpochs times, and returns the median time.
// A template rather than std::function, so the per-block call can inline and costs nothing in the measurement (#83).
template <typename PerBlock>
double measure(ankerl::nanobench::Bench& bench, const char* name, const std::vector<float>& input, PerBlock&& perBlock)
{
    std::vector<float> out(input.size());
    const auto run = [&] {
        for (int pos = 0; pos < kSamples; pos += kBlock)
            perBlock(input.data() + pos, out.data() + pos, kBlock);
        ankerl::nanobench::doNotOptimizeAway(out[static_cast<std::size_t>(kSamples - 1)]);
    };
    // Warm the actual kernel for a fixed wall time; one iteration is too short for small entries (#165).
    const auto warmUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
        run();
    } while (std::chrono::steady_clock::now() < warmUntil);
    bench.run(name, run);
    return bench.results().back().median(ankerl::nanobench::Result::Measure::elapsed);
}

// Costliest callback of a block whose work repeats every `period` samples (the cab's 1024 stage): every block is timed
// over kPeakCycles periods, the median is taken per block phase (robust to preemption), and the worst phase wins.
// Returned as the time for 1 s of audio if every block cost that much, so CPU % reads as the peak share of a block.
inline double peakCallback(const std::vector<float>& input,
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

} // namespace osp::bench
