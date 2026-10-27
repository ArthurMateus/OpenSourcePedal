#include "osp/dsp/Tremolo.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
float clampOrMin(float v, float lo, float hi) noexcept
{
    return std::isnan(v) ? lo : std::clamp(v, lo, hi);
}
} // namespace

Tremolo::Tremolo() noexcept
{
    lfo_.setRateHz(0.8f);
    depth_.reset(0.5f);
}

void Tremolo::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    const double g = std::tan(Lfo::kTwoPi * 0.5 * kCrossoverHz / sampleRate);
    G_ = static_cast<float>(g / (1.0 + g));
    lfo_.prepare(sampleRate);
    mix_.prepare(sampleRate, 0.020);
    width_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    depth_.prepare(sampleRate, 0.020);
    reset();
}

void Tremolo::reset() noexcept
{
    left_ = Side{};
    right_ = Side{};
    lfo_.reset(0.0);
    mix_.reset(mix_.target());
    width_.reset(width_.target());
    enabled_.reset(enabled_.target());
    depth_.reset(depth_.target());
}

void Tremolo::setRateHz(float hz) noexcept
{
    lfo_.setRateHz(clampOrMin(hz, kMinRateHz, kMaxRateHz));
}
void Tremolo::setDepthPct(float pct) noexcept
{
    depth_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Tremolo::setMixPct(float pct) noexcept
{
    mix_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Tremolo::setWidthPct(float pct) noexcept
{
    width_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}

float Tremolo::processSide(Side& side, float x, double phase, float depth) const noexcept
{
    // Two TPT one-pole low-passes: v = G·(u − s), lp = v + s, s' = lp + v.
    const float v1 = G_ * (x - side.s1), lp1 = v1 + side.s1;
    side.s1 = lp1 + v1;
    const float v2 = G_ * (lp1 - side.s2), low = v2 + side.s2;
    side.s2 = low + v2;
    const float lfo = Lfo::sine(phase);
    return lowGain(lfo, depth) * low + highGain(lfo, depth) * (x - low);
}

void Tremolo::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    for (int n = 0; n < numSamples; ++n) {
        const float l = std::isfinite(left[n]) ? left[n] : 0.0f, r = std::isfinite(right[n]) ? right[n] : 0.0f;
        const double phase = lfo_.next();
        const double rightOffset = 0.5 * width_.next();
        const float depth = depth_.next();
        const float wetL = processSide(left_, l, phase, depth);
        const float wetR = processSide(right_, r, phase + rightOffset, depth);
        const float g = mix_.next() * enabled_.next();
        left[n] = l + g * (wetL - l); // g == 0: exactly the dry signal
        right[n] = r + g * (wetR - r);
    }
}

} // namespace osp::dsp
