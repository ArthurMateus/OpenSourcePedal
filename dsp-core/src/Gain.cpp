#include "osp/dsp/Gain.h"

#include <cmath>

namespace osp::dsp {

void Gain::prepare(double sampleRate, int /*maxBlockSize*/,
                   double rampSeconds) noexcept {
  const auto samples = static_cast<int>(std::lround(sampleRate * rampSeconds));
  rampLength_ = samples > 0 ? samples : 1;
  reset();
}

void Gain::reset() noexcept {
  current_ = target_;
  step_ = 0.0f;
  rampRemaining_ = 0;
}

void Gain::setGainDb(float gainDb) noexcept {
  const float target = std::pow(10.0f, gainDb / 20.0f);
  if (target == target_)
    return;
  target_ = target;
  if (!isPrepared()) {
    current_ = target_;
    return;
  }
  rampRemaining_ = rampLength_;
  step_ = (target_ - current_) / static_cast<float>(rampLength_);
}

void Gain::process(const float *in, float *out, int numSamples) noexcept {
  if (numSamples <= 0)
    return;
  if (!isPrepared()) {
    for (int i = 0; i < numSamples; ++i)
      out[i] = 0.0f;
    return;
  }

  int i = 0;
  for (; i < numSamples && rampRemaining_ > 0; ++i) {
    // Computed from the remaining count, not accumulated, so the ramp never
    // drifts past the target.
    --rampRemaining_;
    current_ = target_ - step_ * static_cast<float>(rampRemaining_);
    out[i] = in[i] * current_;
  }
  for (; i < numSamples; ++i)
    out[i] = in[i] * current_;
}

} // namespace osp::dsp
