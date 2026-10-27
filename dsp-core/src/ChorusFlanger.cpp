#include "osp/dsp/ChorusFlanger.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
float clampOrMin(float v, float lo, float hi) noexcept
{
    return std::isnan(v) ? lo : std::clamp(v, lo, hi);
}
constexpr float kLongestMs = 17.0f; // chorusWide 12 + 4 ms, plus a margin
} // namespace

ChorusFlanger::VoicingSpec ChorusFlanger::spec(Voicing v) noexcept
{
    switch (v) {
    case Voicing::chorus80s:
        return {7.0f, 3.0f, 5000.0f, 1, 0.5f};
    case Voicing::flanger:
        return {kFlangerMinMs, kFlangerSweepMs, 0.0f, 1, 0.5f};
    case Voicing::chorusWide:
    default:
        return {12.0f, 4.0f, 0.0f, 2, 0.25f};
    }
}

ChorusFlanger::ChorusFlanger() noexcept
{
    lfo_.setRateHz(0.8f);
    depth_.reset(0.5f);
}

void ChorusFlanger::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    lineL_.prepare(kLongestMs * 1e-3 * sampleRate);
    lineR_.prepare(kLongestMs * 1e-3 * sampleRate);
    lfo_.prepare(sampleRate);
    mix_.prepare(sampleRate, 0.020);
    width_.prepare(sampleRate, 0.020);
    feedback_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    depth_.prepare(sampleRate, 0.020);
    reset();
}

void ChorusFlanger::reset() noexcept
{
    lineL_.reset();
    lineR_.reset();
    lfo_.reset(0.0);
    lpL_ = lpR_ = 0.0f;
    mix_.reset(mix_.target());
    width_.reset(width_.target());
    feedback_.reset(feedback_.target());
    enabled_.reset(enabled_.target());
    depth_.reset(depth_.target());
}

void ChorusFlanger::setRateHz(float hz) noexcept
{
    lfo_.setRateHz(clampOrMin(hz, kMinRateHz, kMaxRateHz));
}
void ChorusFlanger::setDepthPct(float pct) noexcept
{
    depth_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void ChorusFlanger::setMixPct(float pct) noexcept
{
    mix_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void ChorusFlanger::setFeedbackPct(float pct) noexcept
{
    feedback_.setTarget(clampOrMin(pct, -kMaxFeedbackPct, kMaxFeedbackPct) / 100.0f);
}
void ChorusFlanger::setWidthPct(float pct) noexcept
{
    width_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}

void ChorusFlanger::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    const VoicingSpec s = spec(voicing_);
    const bool flanger = voicing_ == Voicing::flanger;
    const double toSamples = 1e-3 * sampleRate_;
    // One-pole low-pass y += a·(x − y), a = 1 − exp(−2π fc / fs) (Smith, "Introduction to Digital Filters").
    const float lpA =
        s.wetLowPassHz > 0.0f ? static_cast<float>(1.0 - std::exp(-Lfo::kTwoPi * s.wetLowPassHz / sampleRate_)) : 1.0f;
    const float voiceGain = 1.0f / static_cast<float>(s.voices);
    for (int n = 0; n < numSamples; ++n) {
        const float l = std::isfinite(left[n]) ? left[n] : 0.0f, r = std::isfinite(right[n]) ? right[n] : 0.0f;
        const double phase = lfo_.next();
        const double depth = depth_.next();
        const double rightOffset = width_.next() * s.maxWidthCycles;
        const float fb = feedback_.next();
        float wetL = 0.0f, wetR = 0.0f;
        if (flanger) {
            // Read before the write, one sample shorter, so the feedback loop is exactly τ long (comb peaks at k/τ).
            const double sweep = depth * s.depthMs * 0.5;
            wetL = lineL_.read((s.baseMs + sweep * (1.0 + Lfo::sine(phase))) * toSamples - 1.0);
            wetR = lineR_.read((s.baseMs + sweep * (1.0 + Lfo::sine(phase + rightOffset))) * toSamples - 1.0);
            lineL_.push(l + fb * std::clamp(wetL, -kLoopLimit, kLoopLimit));
            lineR_.push(r + fb * std::clamp(wetR, -kLoopLimit, kLoopLimit));
        } else {
            lineL_.push(l);
            lineR_.push(r);
            const double m = depth * s.depthMs;
            for (int v = 0; v < s.voices; ++v) {
                const double p = phase + 0.5 * v;
                wetL += lineL_.read((s.baseMs + m * Lfo::sine(p)) * toSamples);
                wetR += lineR_.read((s.baseMs + m * Lfo::sine(p + rightOffset)) * toSamples);
            }
            lpL_ += lpA * (voiceGain * wetL - lpL_);
            lpR_ += lpA * (voiceGain * wetR - lpR_);
            wetL = lpL_;
            wetR = lpR_;
        }
        const float g = mix_.next() * enabled_.next();
        left[n] = l + g * (wetL - l); // g == 0: exactly the dry signal
        right[n] = r + g * (wetR - r);
    }
}

} // namespace osp::dsp
