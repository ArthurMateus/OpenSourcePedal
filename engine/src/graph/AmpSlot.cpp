#include "osp/engine/AmpSlot.h"

#include <cmath>

namespace osp::engine {

void AmpStub::prepare(double sampleRate, int /*maxBlockSize*/, dsp::OversamplingQuality /*quality*/)
{
    level_.prepare(sampleRate, 0.020); // parameters.md §1.4: gain 20 ms linear ramp
    // The hard-bypass pad is round(D_os1 + D_slot) − round(D_os1) <= ceil(D_slot) + 1.
    delay_.prepare(static_cast<int>(std::ceil(testDelay_)) + 4);
    reset();
}

void AmpStub::reset() noexcept
{
    level_.reset(level_.target());
    delay_.reset();
}

void AmpStub::setParameters(const AmpParams& params) noexcept
{
    const float dB = std::isfinite(params.levelDb) ? params.levelDb : -24.0f;
    level_.setTarget(std::pow(10.0f, 0.05f * (dB < -24.0f ? -24.0f : (dB > 12.0f ? 12.0f : dB))));
}

void AmpStub::setBypassed(bool bypassed, int padSamples) noexcept
{
    bypassed_ = bypassed;
    pad_ = padSamples > 0 ? padSamples : 0;
}

void AmpStub::process(float* io, int numSamples) noexcept
{
    // FractionalDelay::read(d) after push() is a delay of d − 1 samples, exact at an integer d.
    const float d = 1.0f + static_cast<float>(bypassed_ ? pad_ : testDelay_);
    if (d > 1.0f) {
        for (int i = 0; i < numSamples; ++i) {
            delay_.push(io[i]);
            io[i] = delay_.read(d);
        }
    }
    if (bypassed_)
        return;
    for (int i = 0; i < numSamples; ++i)
        io[i] *= level_.next();
}

} // namespace osp::engine
