#include "osp/dsp/DriveRegion.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

int DriveRegion::factorFor(double sampleRate, OversamplingQuality quality) noexcept
{
    if (quality == OversamplingQuality::hq)
        return 8;
    return sampleRate <= 50000.0 ? 4 : 2;
}

DriveRegion::DriveRegion()
{
    for (auto& s : slots_)
        s.setEnabled(false);
}

void DriveRegion::prepare(double sampleRate, int maxBlockSize, OversamplingQuality quality)
{
    const int factor = factorFor(sampleRate, quality);
    const int maxBlock = std::max(maxBlockSize, 1);
    const auto filter = quality == OversamplingQuality::hq ? OversamplingFilter::fir : OversamplingFilter::iir;
    const bool qualitySwitch = isPrepared() && quality != quality_;
    quality_ = quality;
    oversampler_.prepare(sampleRate, maxBlock, factor, filter);
    for (auto& s : slots_)
        s.prepare(sampleRate * factor, maxBlock * factor);
    // FIR: pad the slots' delay up to a whole base sample, so D stays an integer (ADR-0005 D4, Amendment A2).
    constexpr int slotDelay = kNumSlots * static_cast<int>(DriveStage::kDelaySamples);
    padSamples_ = filter == OversamplingFilter::fir ? (factor - slotDelay % factor) % factor : 0;
    pad_.prepare(padSamples_);
    pad_.setDelay(padSamples_);
    delay_ = oversampler_.getMeasuredDelaySamples() + static_cast<double>(slotDelay + padSamples_) / factor;
    latency_ = static_cast<int>(std::lround(delay_));
    bypassDelay_.prepare(latency_);
    bypassDelay_.setDelay(latency_);
    fadeLength_ = qualitySwitch ? static_cast<int>(std::lround(kSwitchFadeMs * 1e-3 * sampleRate)) : 0;
    reset();
}

void DriveRegion::reset() noexcept
{
    oversampler_.reset();
    for (auto& s : slots_)
        s.reset();
    bypassDelay_.reset();
    pad_.reset();
    fadePos_ = 0;
}

void DriveRegion::process(const float* in, float* out, int numSamples) noexcept
{
    if (!isPrepared()) {
        std::fill_n(out, std::max(numSamples, 0), 0.0f);
        return;
    }
    if (bypassed_) {
        bypassDelay_.process(in, out, numSamples);
        return;
    }
    oversampler_.process(in, out, numSamples, [this](float* os, int osN) {
        for (auto& s : slots_)
            s.process(os, osN);
        pad_.process(os, os, osN);
    });
    // Silent until the new chain's latency has passed (FIR pre-ringing included), then a linear ramp.
    for (int i = 0; fadeLength_ > 0 && i < numSamples && fadePos_ < latency_ + fadeLength_; ++i, ++fadePos_)
        out[i] *= static_cast<float>(std::max(fadePos_ - latency_, 0)) / static_cast<float>(fadeLength_);
}

} // namespace osp::dsp
