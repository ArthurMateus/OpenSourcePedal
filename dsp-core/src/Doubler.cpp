#include "osp/dsp/Doubler.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;

float clampOrMin(float v, float lo, float hi) noexcept
{
    return std::isnan(v) ? lo : std::clamp(v, lo, hi);
}

// Delay-modulation depth in seconds for a peak pitch deviation of `cents` with an LFO at `hz` (see the header).
double depthSeconds(double cents, double hz) noexcept
{
    return (std::exp2(cents / 1200.0) - 1.0) / (kTwoPi * hz);
}
} // namespace

Doubler::Doubler() noexcept
{
    delayMs_.reset(18.0f);
    detune_.reset(8.0f);
}

void Doubler::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    // Longest read: the maximum delay, the deepest modulation (25 cents at the slower LFO) and the 4-point margin.
    const double maxSeconds = kMaxDelayMs / 1000.0 + depthSeconds(kMaxDetuneCents, kRateAHz);
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(std::ceil(maxSeconds * sampleRate)) + 8)
        size <<= 1;
    ringL_.assign(size, 0.0f);
    ringR_.assign(size, 0.0f);
    mask_ = static_cast<unsigned>(size - 1);
    amount_.prepare(sampleRate, 0.020);
    width_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    delayMs_.prepare(sampleRate, 0.080);
    detune_.prepare(sampleRate, 0.020);
    reset();
}

void Doubler::reset() noexcept
{
    std::fill(ringL_.begin(), ringL_.end(), 0.0f);
    std::fill(ringR_.begin(), ringR_.end(), 0.0f);
    write_ = 0;
    phaseA_ = 0.0;
    phaseB_ = 0.5;
    amount_.reset(amount_.target());
    width_.reset(width_.target());
    enabled_.reset(enabled_.target());
    delayMs_.reset(delayMs_.target());
    detune_.reset(detune_.target());
}

void Doubler::setAmountPct(float pct) noexcept
{
    amount_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Doubler::setWidthPct(float pct) noexcept
{
    width_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Doubler::setDelayMs(float ms) noexcept
{
    delayMs_.setTarget(clampOrMin(ms, kMinDelayMs, kMaxDelayMs));
}
void Doubler::setDetuneCents(float c) noexcept
{
    detune_.setTarget(clampOrMin(c, 0.0f, kMaxDetuneCents));
}

float Doubler::read(const std::vector<float>& ring, double delaySamples) const noexcept
{
    // write_ is the index of the newest sample. Catmull-Rom between x[n - i] and x[n - i - 1] (older), t = frac.
    const auto i = static_cast<unsigned>(delaySamples);
    const auto t = static_cast<float>(delaySamples - i);
    const float p0 = ring[(write_ - i + 1) & mask_], p1 = ring[(write_ - i) & mask_],
                p2 = ring[(write_ - i - 1) & mask_], p3 = ring[(write_ - i - 2) & mask_];
    return p1 +
           0.5f * t * ((p2 - p0) + t * ((2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) + t * (3.0f * (p1 - p2) + p3 - p0)));
}

void Doubler::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    const double incA = kRateAHz / sampleRate_, incB = kRateBHz / sampleRate_;
    for (int n = 0; n < numSamples; ++n) {
        const float l = std::isfinite(left[n]) ? left[n] : 0.0f, r = std::isfinite(right[n]) ? right[n] : 0.0f;
        ++write_;
        ringL_[write_ & mask_] = l;
        ringR_[write_ & mask_] = r;

        const double base = delayMs_.next() * 1e-3 * sampleRate_;
        const double cents = detune_.next();
        const double tauA = base + depthSeconds(cents, kRateAHz) * sampleRate_ * std::sin(kTwoPi * phaseA_);
        const double tauB = base + depthSeconds(cents, kRateBHz) * sampleRate_ * std::sin(kTwoPi * phaseB_);
        phaseA_ += incA;
        phaseB_ += incB;
        phaseA_ -= std::floor(phaseA_);
        phaseB_ -= std::floor(phaseB_);
        const float va = read(ringL_, tauA), vb = read(ringR_, tauB);

        const float w = width_.next();
        const float g = amount_.next() * enabled_.next() * kWetGain;
        const float gNear = 0.5f * (1.0f + w), gFar = 0.5f * (1.0f - w); // A left, B right
        left[n] = l + g * (gNear * va + gFar * vb);                     // g == 0: exactly the dry signal
        right[n] = r + g * (gFar * va + gNear * vb);
    }
}

} // namespace osp::dsp
