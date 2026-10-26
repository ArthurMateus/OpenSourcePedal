#include "osp/dsp/Gain.h"

#include <cmath>

namespace osp::dsp {

void Gain::prepare(double sampleRate, int /*maxBlockSize*/, double rampSeconds) noexcept
{
    ramp_.prepare(sampleRate, rampSeconds);
}

void Gain::reset() noexcept
{
    ramp_.reset(ramp_.target());
}

void Gain::setGainDb(float gainDb) noexcept
{
    ramp_.setTarget(std::pow(10.0f, gainDb / 20.0f));
}

void Gain::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        for (int i = 0; i < numSamples; ++i)
            out[i] = 0.0f;
        return;
    }
    // Split loop: the ramp segment per sample, then a plain constant-gain loop that vectorises (osp-bench, #82).
    int i = 0;
    for (; i < numSamples && ramp_.isRamping(); ++i)
        out[i] = in[i] * ramp_.next();
    const float g = ramp_.current();
    for (; i < numSamples; ++i)
        out[i] = in[i] * g;
}

} // namespace osp::dsp
