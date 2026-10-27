#include "osp/dsp/delay/LoopSaturator.h"

#include "osp/dsp/oversampling/Adaa.h"

#include <cmath>

namespace osp::dsp {

void LoopSaturator::prepare(double sampleRate, int maxChunk)
{
    os_.prepare(sampleRate, maxChunk, sampleRate < 88200.0 ? 2 : 1);
    delay_ = os_.getMeasuredDelaySamples() + 0.5 / os_.getFactor();
    reset();
}

void LoopSaturator::reset() noexcept
{
    os_.reset();
    x1_ = f1x1_ = g1_ = b1_ = c_ = 0.0;
}

double LoopSaturator::f(double x) const noexcept
{
    return (std::tanh(g1_ * x + b1_) - c_) / g1_;
}

double LoopSaturator::F1(double x) const noexcept
{
    return (TanhShape::F1(g1_ * x + b1_) / g1_ - c_ * x) / g1_;
}

void LoopSaturator::process(float* x, const float* drive, float bias, int n) noexcept
{
    if (n <= 0 || !os_.isPrepared())
        return;
    float* os = os_.upsample(x, n);
    const int factor = os_.getFactor();
    for (int j = 0; j < n * factor; ++j) {
        const double g = drive[j / factor];
        if (g != g1_ || bias != b1_) { // the shape moved: refresh its constants and x₋₁'s antiderivative under it
            g1_ = g;
            b1_ = bias;
            c_ = std::tanh(bias);
            f1x1_ = F1(x1_);
        }
        const double v = os[j];
        const double f1 = F1(v), dv = v - x1_;
        os[j] = static_cast<float>(std::abs(dv) < kAdaaTolerance ? f(0.5 * (v + x1_)) : (f1 - f1x1_) / dv);
        x1_ = v;
        f1x1_ = f1;
    }
    os_.downsample(x, n);
}

} // namespace osp::dsp
