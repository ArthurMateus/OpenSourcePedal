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
    oversampler_.prepare(sampleRate, maxBlock, factor);
    for (auto& s : slots_)
        s.prepare(sampleRate * factor, maxBlock * factor);
    delay_ = oversampler_.getMeasuredDelaySamples() + kNumSlots * DriveStage::kDelaySamples / factor;
    latency_ = static_cast<int>(std::lround(delay_));
    bypassDelay_.prepare(latency_);
    bypassDelay_.setDelay(latency_);
    reset();
}

void DriveRegion::reset() noexcept
{
    oversampler_.reset();
    for (auto& s : slots_)
        s.reset();
    bypassDelay_.reset();
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
    });
}

} // namespace osp::dsp
