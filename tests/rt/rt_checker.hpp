#pragma once

// Test-side API of the RT checker (realtime-rules.md §2, ADR-0007 REQ-TEST-006). The hooks behind the
// counters (operator new/delete replacement, MSVC Debug CRT alloc hook) are in rt_checker.cpp, linked into
// osp_tests.

#include "osp/platform/FlushDenormals.h"
#include "osp/rt/AudioThread.h"

#include <cstddef>
#include <span>

namespace osp::rt::test {

ViolationCounters operator-(const ViolationCounters& after, const ViolationCounters& before) noexcept;

struct RtRunResult {
    ViolationCounters violations;  // counted while the callable ran as the audio thread
    bool denormalsFlushed = false; // C4: FTZ + DAZ were on inside the scope
};

// C1 + C4: runs `f` as the audio thread with FTZ/DAZ on, and returns what the checker saw. `f` itself must
// not allocate, so capture by reference and preallocate buffers outside.
template <typename F>
RtRunResult runAsAudioThread(F&& f)
{
    const ViolationCounters before = counters();
    RtRunResult result;
    {
        ScopedFlushDenormals ftz;
        ScopedAudioThread audio;
        result.denormalsFlushed = denormalsFlushed();
        f();
    }
    result.violations = counters() - before;
    return result;
}

// C4 / RT-9: counts non-finite and subnormal samples. Classification is by bit pattern, so it is independent
// of the current FTZ/DAZ state.
struct OutputScan {
    std::size_t nonFinite = 0;
    std::size_t subnormal = 0;
    bool clean() const noexcept { return nonFinite == 0 && subnormal == 0; }
};
OutputScan scanOutput(std::span<const float> samples) noexcept;

} // namespace osp::rt::test

// Catch2 helpers: every process() call in a block test goes through runAsAudioThread and these checks.
#define OSP_REQUIRE_RT_CLEAN(result)                                                                                   \
    do {                                                                                                               \
        const auto& osp_rt_result_ = (result);                                                                         \
        CHECK(osp_rt_result_.violations.allocs == 0u);                                                                 \
        CHECK(osp_rt_result_.violations.frees == 0u);                                                                  \
        CHECK(osp_rt_result_.violations.crtAllocs == 0u);                                                              \
        CHECK(osp_rt_result_.violations.locks == 0u);                                                                  \
        REQUIRE(osp_rt_result_.denormalsFlushed);                                                                      \
        REQUIRE(osp_rt_result_.violations == osp::rt::ViolationCounters{});                                            \
    } while (false)

#define OSP_REQUIRE_OUTPUT_CLEAN(samples)                                                                              \
    do {                                                                                                               \
        const auto osp_scan_ = osp::rt::test::scanOutput(samples);                                                     \
        CHECK(osp_scan_.nonFinite == 0u);                                                                              \
        CHECK(osp_scan_.subnormal == 0u);                                                                              \
        REQUIRE(osp_scan_.clean());                                                                                    \
    } while (false)
