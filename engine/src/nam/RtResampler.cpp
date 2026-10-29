// RtResampler: altered fork of AudioDSPTools' ResamplingContainer/LanczosResampler (MIT; iPlug 2 zlib-style notice;
// LanczosResampler derived from sst-basic-blocks under its MIT/BSD file exception). Original notices, verbatim:
// RtResampler.NOTICE. Source commit and the list of changes: RtResampler.h. Recorded in THIRD_PARTY.md.

#include "RtResampler.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <numeric>

namespace osp::engine::nam {
namespace {

using LR = LanczosResampler;

// Upstream's static sTable/sDeltaTable, built once. A function-local static initialises thread-safely ([stmt.dcl]),
// unlike upstream's unsynchronised `sTablesInitialized` flag. Only prepare() calls this (control thread).
struct KernelTables {
    float table[LR::kTablePoints + 1][LR::kFilterWidth];
    float delta[LR::kTablePoints + 1][LR::kFilterWidth];

    KernelTables()
    {
        constexpr double pi = std::numbers::pi;
        constexpr double a = LR::kA;
        const auto kernel = [&](double x) {
            if (std::fabs(x) < 1e-7)
                return 1.0;
            return a * std::sin(pi * x) * std::sin(pi * x / a) / (pi * pi * x * x);
        };
        constexpr double dx = 1.0 / LR::kTablePoints;
        for (int t = 0; t <= LR::kTablePoints; ++t)
            for (int i = 0; i < LR::kFilterWidth; ++i)
                table[t][i] = static_cast<float>(kernel(dx * t + i - a));
        for (int t = 0; t < LR::kTablePoints; ++t)
            for (int i = 0; i < LR::kFilterWidth; ++i)
                delta[t][i] = table[t + 1][i] - table[t][i];
        for (int i = 0; i < LR::kFilterWidth; ++i)
            delta[LR::kTablePoints][i] = delta[0][i]; // wrap at the end: the delta is the same
    }
};

const KernelTables& kernelTables()
{
    static const KernelTables tables;
    return tables;
}

} // namespace

void LanczosResampler::prepare(std::int64_t inputRate, std::int64_t outputRate, std::size_t capacity)
{
    const KernelTables& t = kernelTables();
    table_ = &t.table[0][0];
    delta_ = &t.delta[0][0];
    const std::int64_t g = std::gcd(inputRate, outputRate);
    phaseInIncr_ = outputRate / g; // phase in samples, scaled by the denominator: one input sample = 1
    phaseDenominator_ = phaseInIncr_;
    phaseOutIncr_ = inputRate / g;
    // Power of two for the index mask; at least the filter width plus headroom.
    capacity_ = std::bit_ceil(std::max<std::size_t>(capacity, 4 * kFilterWidth));
    buffer_.assign(2 * capacity_, 0.0f); // RT-12: assign() writes every element
    clear();
}

void LanczosResampler::clear() noexcept OSP_RT_NONBLOCKING
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writePos_ = 0;
    phaseIn_ = 0;
    phaseOut_ = 0;
}

std::size_t LanczosResampler::numSamplesRequiredFor(std::size_t numOutputSamples) const noexcept OSP_RT_NONBLOCKING
{
    // res > (A + 1) - (phaseIn - phaseOut - phaseOutIncr * nOut) / denominator   (upstream derivation)
    const double res =
        kA + 1.0 -
        static_cast<double>(phaseIn_ - phaseOut_ - phaseOutIncr_ * static_cast<std::int64_t>(numOutputSamples)) /
            static_cast<double>(phaseDenominator_);
    return static_cast<std::size_t>(std::max(res + 1.0, 0.0));
}

void LanczosResampler::push(const float* in, std::size_t n) noexcept OSP_RT_NONBLOCKING
{
    const std::size_t mask = capacity_ - 1;
    for (std::size_t s = 0; s < n; ++s) {
        const float x = in != nullptr ? in[s] : 0.0f;
        buffer_[writePos_] = x;
        buffer_[writePos_ + capacity_] = x; // mirrored copy, so a read window never wraps
        writePos_ = (writePos_ + 1) & mask;
        phaseIn_ += phaseInIncr_;
    }
}

std::size_t LanczosResampler::pop(float* out, std::size_t maxSamples) noexcept OSP_RT_NONBLOCKING
{
    std::size_t populated = 0;
    while (populated < maxSamples && (phaseIn_ - phaseOut_) > phaseDenominator_ * (kA + 1)) {
        out[populated++] =
            readSample(static_cast<double>(phaseIn_ - phaseOut_) / static_cast<double>(phaseDenominator_));
        phaseOut_ += phaseOutIncr_;
    }
    return populated;
}

float LanczosResampler::readSample(double xBack) const noexcept OSP_RT_NONBLOCKING
{
    const auto cap = static_cast<std::int64_t>(capacity_);
    const double readPos = static_cast<double>(writePos_) - xBack;
    std::int64_t index = static_cast<std::int64_t>(std::floor(readPos));
    const double frac = 1.0 - (readPos - static_cast<double>(index));
    index = (index + cap) & (cap - 1);
    index += (index <= kA) ? cap : 0; // keep [index - A, index + A) inside the mirrored buffer

    const double tablePos = frac * kTablePoints;
    const int t = static_cast<int>(tablePos);
    const auto tf = static_cast<float>(tablePos - t);
    const float* row = table_ + static_cast<std::ptrdiff_t>(t) * kFilterWidth;
    const float* drow = delta_ + static_cast<std::ptrdiff_t>(t) * kFilterWidth;
    const float* x = buffer_.data() + index;

    float sum = 0.0f;
    for (int i = 0; i < kA; ++i) {
        const float f0 = row[i] + drow[i] * tf;
        const float f1 = row[kA + i] + drow[kA + i] * tf;
        sum += f0 * x[i - kA] + f1 * x[i];
    }
    return sum;
}

bool RtResampler::prepare(double hostSampleRate, double innerSampleRate, int maxBlockSize)
{
    maxBlock_ = 0;
    const std::int64_t host = std::llround(hostSampleRate);
    const std::int64_t inner = std::llround(innerSampleRate);
    if (host <= 0 || inner <= 0 || maxBlockSize < 1)
        return false;

    // Largest inner block for one host block: ceil(maxBlock * inner / host), plus 1 for the phase carry.
    maxInnerBlock_ = static_cast<int>((maxBlockSize * inner + host - 1) / host) + 1;

    // Upstream warm-up sizes: the inner samples the second resampler needs for one output sample, and the host
    // samples the first one needs to produce them. They depend only on the rates (phases start at 0).
    up_.prepare(host, inner, 0);
    down_.prepare(inner, host, 0);
    warmupInner_ = down_.numSamplesRequiredFor(1);
    warmupHost_ = up_.numSamplesRequiredFor(warmupInner_);
    // Round-trip delay. readSample() centres output m of a stage at input index m * in/out - 1 (its fractional
    // weight is kernel(1 - frac)), so each stage adds one of its input samples. With the warm-up (warmupHost_
    // silent host samples in, warmupInner_ inner samples handed on), host output n is centred on host input
    // n - warmupHost_ - 1 - host/inner. Measured by tests/engine/test_rt_resampler.cpp (1 kHz phase delay):
    // 28.919 / 41.838 / 45.000 samples at 44.1 / 88.2 / 96 kHz against a 48 kHz inner rate.
    delay_ = static_cast<double>(warmupHost_) + 1.0 + static_cast<double>(host) / static_cast<double>(inner);
    latency_ = static_cast<int>(std::lround(delay_));

    // Ring size: the unread backlog never exceeds one block (or the warm-up) plus the filter's A + 1 history.
    const auto backlog =
        static_cast<std::size_t>(std::max({maxBlockSize, maxInnerBlock_, static_cast<int>(warmupHost_)})) +
        static_cast<std::size_t>(warmupInner_);
    const std::size_t capacity = 2 * (backlog + 2 * (LanczosResampler::kA + 2));
    up_.prepare(host, inner, capacity);
    down_.prepare(inner, host, capacity);

    const std::size_t innerSize = std::max(static_cast<std::size_t>(maxInnerBlock_), warmupInner_);
    innerIn_.assign(innerSize, 0.0f);
    innerOut_.assign(innerSize, 0.0f);
    maxBlock_ = maxBlockSize;
    reset();
    return true;
}

void RtResampler::reset() noexcept OSP_RT_NONBLOCKING
{
    if (!isPrepared())
        return;
    up_.clear();
    down_.clear();
    // Upstream warm-up: push silence through the first resampler, and feed the second one the inner callable's
    // output for it, which is silence for a causal effect at rest, so the first real block yields a full output.
    up_.push(nullptr, warmupHost_);
    const std::size_t populated = up_.pop(innerIn_.data(), warmupInner_);
    if (populated < warmupInner_) // upstream: throw. Unreachable by construction.
        countError();
    down_.push(nullptr, populated);
    up_.renormalizePhases();
    down_.renormalizePhases();
}

} // namespace osp::engine::nam
