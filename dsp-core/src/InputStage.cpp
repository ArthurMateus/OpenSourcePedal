#include "osp/dsp/InputStage.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

void DcBlocker::prepare(double sampleRate, double cornerHz) noexcept
{
    r = static_cast<float>(std::exp(-2.0 * 3.14159265358979323846 * cornerHz / sampleRate));
    reset();
}

void InputStage::prepare(double sampleRate, int maxBlockSize) noexcept
{
    trim_.prepare(sampleRate, maxBlockSize);
    dc_.prepare(sampleRate, kDcCornerHz);
    reset();
}

void InputStage::reset() noexcept
{
    trim_.reset();
    dc_.reset();
}

void InputStage::setGainDb(float dB) noexcept
{
    trim_.setGainDb(std::isnan(dB) ? kMinGainDb : std::clamp(dB, kMinGainDb, kMaxGainDb));
}

void InputStage::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (bypassed_) {
        if (in != out)
            std::copy(in, in + numSamples, out);
        return;
    }
    if (!isPrepared()) {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }
    for (int i = 0; i < numSamples; ++i)
        out[i] = std::isfinite(in[i]) ? in[i] : 0.0f;
    trim_.process(out, out, numSamples);
    for (int i = 0; i < numSamples; ++i)
        out[i] = dc_.tick(out[i]);
}

} // namespace osp::dsp
