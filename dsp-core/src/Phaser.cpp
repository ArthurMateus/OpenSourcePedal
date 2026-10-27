#include "osp/dsp/Phaser.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
float clampOrMin(float v, float lo, float hi) noexcept
{
    return std::isnan(v) ? lo : std::clamp(v, lo, hi);
}
constexpr float kLoopLimit = 8.0f; // feedback-state bound, never reached with |input| <= 1
} // namespace

Phaser::Phaser() noexcept
{
    lfo_.setRateHz(0.8f);
    depth_.reset(0.5f);
}

double Phaser::phaserCentreHz() noexcept
{
    return std::sqrt(kPhaserLowHz * kPhaserHighHz);
}

void Phaser::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    cellAttack_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    cellRelease_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.025 * sampleRate)));
    lfo_.prepare(sampleRate);
    mix_.prepare(sampleRate, 0.020);
    width_.prepare(sampleRate, 0.020);
    feedback_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    depth_.prepare(sampleRate, 0.020);
    reset();
}

void Phaser::reset() noexcept
{
    left_ = Side{};
    right_ = Side{};
    lfo_.reset(0.0);
    mix_.reset(mix_.target());
    width_.reset(width_.target());
    feedback_.reset(feedback_.target());
    enabled_.reset(enabled_.target());
    depth_.reset(depth_.target());
}

void Phaser::setRateHz(float hz) noexcept
{
    lfo_.setRateHz(clampOrMin(hz, kMinRateHz, kMaxRateHz));
}
void Phaser::setDepthPct(float pct) noexcept
{
    depth_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Phaser::setMixPct(float pct) noexcept
{
    mix_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void Phaser::setFeedbackPct(float pct) noexcept
{
    feedback_.setTarget(clampOrMin(pct, -kMaxFeedbackPct, kMaxFeedbackPct) / 100.0f);
}
void Phaser::setWidthPct(float pct) noexcept
{
    width_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}

float Phaser::processSide(Side& side, float x, double phase, float depth, float fb) noexcept
{
    const double piOverFs = Lfo::kTwoPi * 0.5 / sampleRate_, maxHz = 0.45 * sampleRate_;
    // One TPT allpass stage: v = G·(u − s), lp = v + s, s' = lp + v, y = 2·lp − u, G = g / (1 + g).
    auto stage = [](float& s, float u, float G) noexcept {
        const float v = G * (u - s), lp = v + s;
        s = lp + v;
        return 2.0f * lp - u;
    };
    auto coeff = [&](double hz) noexcept {
        const auto g = static_cast<float>(std::tan(piOverFs * std::min(hz, maxHz)));
        return g / (1.0f + g);
    };
    if (voicing_ == Voicing::phaser) {
        const double fc = kPhaserLowHz * std::pow(kPhaserHighHz / kPhaserLowHz, 0.5 + 0.5 * depth * Lfo::sine(phase));
        const float G = coeff(fc);
        float u = x + fb * side.last;
        for (int i = 0; i < kPhaserStages; ++i)
            u = stage(side.s[static_cast<std::size_t>(i)], u, G);
        side.last = std::clamp(u, -kLoopLimit, kLoopLimit);
        return u;
    }
    const float lamp = 0.25f * (1.0f + Lfo::sine(phase)) * (1.0f + Lfo::sine(phase));
    side.cell += (lamp > side.cell ? cellAttack_ : cellRelease_) * (lamp - side.cell);
    const double base = kVibeLowHz * std::pow(kVibeSpan, static_cast<double>(depth * side.cell));
    float u = x;
    for (std::size_t i = 0; i < kVibeRatios.size(); ++i)
        u = stage(side.s[i], u, coeff(base * kVibeRatios[i]));
    return u;
}

void Phaser::process(float* left, float* right, int numSamples) noexcept
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
        const float depth = depth_.next(), fb = feedback_.next();
        const float wetL = processSide(left_, l, phase, depth, fb);
        const float wetR = processSide(right_, r, phase + rightOffset, depth, fb);
        const float g = mix_.next() * enabled_.next();
        left[n] = l + g * (wetL - l); // g == 0: exactly the dry signal
        right[n] = r + g * (wetR - r);
    }
}

} // namespace osp::dsp
