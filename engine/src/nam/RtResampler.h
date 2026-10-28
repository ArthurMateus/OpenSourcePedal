#pragma once

// RtResampler: a real-time-safe fork of AudioDSPTools' ResamplingContainer + LanczosResampler (ADR-0002 D4).
//
// Source: https://github.com/sdatkinson/AudioDSPTools at commit 0827c6c2fc0deced568536142ea86f189e0b98a1 (the
// NeuralAmpModelerCore v0.6.0 submodule), files dsp/ResamplingContainer/ResamplingContainer.h and
// dsp/ResamplingContainer/Dependencies/LanczosResampler.h. Recorded in THIRD_PARTY.md.
//
// ALTERED SOURCE VERSION (marked as required by the iPlug 2 licence below). Changes by OpenSourcePedal, 2026-10-28:
// - mono float only (NCHANS = 1, T = float), Lanczos A = 12 as upstream, scalar path only (upstream's SIMD path is
//   marked unchecked);
// - the std::function block callback is a template callable `inner(const float* in, float* out, int n)`;
// - both `throw` branches and the std::cerr messages are replaced by an std::atomic<uint32_t> error counter and a
//   zero-filled output; the control thread polls errorCount();
// - every buffer (the ring buffers too, upstream's fixed 131072-sample arrays) is sized in prepare() from
//   maxBlockSize and the rate ratio, so the error branches are unreachable for 0 <= n <= maxBlockSize;
// - the kernel tables are built once behind a thread-safe function-local static, not a racy `bool` flag;
// - phase numerators are int64_t (Windows `long` is 32-bit); reset() replays the silent warm-up in O(state);
// - the reported latency is the measured/derived round-trip delay, not upstream's warm-up length (see prepare()).
// The algorithm (rational phase accumulators, table-interpolated Lanczos kernel, latency warm-up) is unchanged.
//
// The original licence (MIT, Copyright (c) 2023 Steven Atkinson) and the original file headers (the iPlug 2
// zlib-style notice and the sst-basic-blocks MIT/BSD file exception for the Lanczos part) are kept verbatim in
// RtResampler.NOTICE next to this file, as those licences require. This notice must ship with the source.

#include "osp/rt/AudioThread.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace osp::engine::nam {

// One direction of mono Lanczos resampling (upstream LanczosResampler<float, 1, 12>).
class LanczosResampler {
public:
    static constexpr int kA = 12;
    static constexpr int kFilterWidth = 2 * kA;
    static constexpr int kTablePoints = 8192;

    // Control thread. Integer rates (Hz); `capacity` is the ring size, rounded up to a power of two.
    void prepare(std::int64_t inputRate, std::int64_t outputRate, std::size_t capacity);
    void clear() noexcept OSP_RT_NONBLOCKING;

    std::size_t numSamplesRequiredFor(std::size_t numOutputSamples) const noexcept OSP_RT_NONBLOCKING;
    void push(const float* in, std::size_t n) noexcept OSP_RT_NONBLOCKING; // in == nullptr pushes silence
    std::size_t pop(float* out, std::size_t maxSamples) noexcept OSP_RT_NONBLOCKING;
    void renormalizePhases() noexcept OSP_RT_NONBLOCKING
    {
        phaseIn_ -= phaseOut_;
        phaseOut_ = 0;
    }

private:
    float readSample(double xBack) const noexcept OSP_RT_NONBLOCKING;

    const float* table_ = nullptr; // [kTablePoints + 1][kFilterWidth], shared by every instance
    const float* delta_ = nullptr;
    std::vector<float> buffer_; // 2 * capacity_: every sample is written twice, so reads never wrap
    std::size_t capacity_ = 0;
    std::size_t writePos_ = 0;
    // Rational phases (upstream issue 15): in samples * phaseDenominator_.
    std::int64_t phaseIn_ = 0, phaseOut_ = 0, phaseInIncr_ = 1, phaseOutIncr_ = 1, phaseDenominator_ = 1;
};

// Host rate -> inner rate -> host rate around a mono block callable (upstream ResamplingContainer).
class RtResampler {
public:
    // Control thread; allocates. Rates are rounded to whole Hz. Returns false (and stays unprepared) for a rate <= 0
    // or maxBlockSize < 1.
    bool prepare(double hostSampleRate, double innerSampleRate, int maxBlockSize);

    // Audio thread, O(state): back to the state right after prepare() (silent history).
    void reset() noexcept OSP_RT_NONBLOCKING;

    // Audio thread. `inner(const float* in, float* out, int n)` runs at the inner rate with
    // 1 <= n <= getMaxInnerBlockSize(); it must not write past `out + n`. `in` and `out` must not alias.
    // n <= 0 does nothing. Before prepare(), `out` is zero-filled. n > maxBlockSize counts an error and zero-fills.
    template <typename Inner>
    void process(const float* in, float* out, int numSamples, Inner&& inner) noexcept OSP_RT_NONBLOCKING;

    // Delay of the resampling round trip in host samples (the inner callable's own latency excluded): exact, and
    // rounded for the host. Upstream's GetLatency() is the warm-up length only, 1 + host/inner samples short.
    double getDelaySamples() const noexcept OSP_RT_NONBLOCKING { return delay_; }
    int getLatencySamples() const noexcept OSP_RT_NONBLOCKING { return latency_; }
    int getMaxInnerBlockSize() const noexcept OSP_RT_NONBLOCKING { return maxInnerBlock_; }
    bool isPrepared() const noexcept OSP_RT_NONBLOCKING { return maxBlock_ > 0; }
    // Any thread. Upstream's throw/cerr branches; stays 0 for valid use (REQ-NAM-002).
    std::uint32_t errorCount() const noexcept { return errors_.load(std::memory_order_relaxed); }

private:
    static constexpr int kMaxInnerCallsPerBlock = 8; // RT-7 bound; 2 suffice for any ratio (see process())
    void countError() noexcept OSP_RT_NONBLOCKING { errors_.fetch_add(1, std::memory_order_relaxed); }
    static void zeroFill(float* out, int n) noexcept OSP_RT_NONBLOCKING { std::fill(out, out + n, 0.0f); }

    LanczosResampler up_, down_; // host -> inner, inner -> host
    std::vector<float> innerIn_, innerOut_;
    int maxBlock_ = 0, maxInnerBlock_ = 0, latency_ = 0;
    std::size_t warmupHost_ = 0, warmupInner_ = 0;
    double delay_ = 0.0;
    std::atomic<std::uint32_t> errors_{0};
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "RT-6");
};

template <typename Inner>
void RtResampler::process(const float* in, float* out, int numSamples, Inner&& inner) noexcept OSP_RT_NONBLOCKING
{
    if (numSamples <= 0)
        return;
    if (!isPrepared() || numSamples > maxBlock_) {
        if (isPrepared())
            countError();
        zeroFill(out, numSamples);
        return;
    }
    up_.push(in, static_cast<std::size_t>(numSamples));
    // Upstream loop: run the inner callable on everything the first resampler can produce. Each call drains up to
    // maxInnerBlock_ samples, which covers a whole block, so the second call (if any) only takes the phase carry.
    const auto maxInner = static_cast<std::size_t>(maxInnerBlock_);
    for (int calls = 0; up_.numSamplesRequiredFor(1) == 0; ++calls) {
        const std::size_t n1 = up_.pop(innerIn_.data(), maxInner);
        if (n1 == 0 || calls == kMaxInnerCallsPerBlock) { // upstream: throw. Unreachable by construction.
            countError();
            break;
        }
        inner(static_cast<const float*>(innerIn_.data()), innerOut_.data(), static_cast<int>(n1));
        down_.push(innerOut_.data(), n1);
    }
    const std::size_t n2 = down_.pop(out, static_cast<std::size_t>(numSamples));
    if (n2 < static_cast<std::size_t>(numSamples)) { // upstream: std::cerr + hold. Unreachable by construction.
        countError();
        zeroFill(out + n2, numSamples - static_cast<int>(n2));
    }
    up_.renormalizePhases();
    down_.renormalizePhases();
}

} // namespace osp::engine::nam
