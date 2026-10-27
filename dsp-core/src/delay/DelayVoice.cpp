#include "osp/dsp/delay/DelayVoice.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
float clampOrMin(float v, float lo, float hi) noexcept
{
    return std::isnan(v) ? lo : std::clamp(v, lo, hi);
}
float finiteOrZero(float v) noexcept
{
    return std::isfinite(v) ? v : 0.0f;
}
float limit(float v) noexcept
{
    return std::clamp(v, -DelayVoice::kLoopLimit, DelayVoice::kLoopLimit);
}
} // namespace

float DelayVoice::OnePole::lp(float x, float G) noexcept
{
    const float v = (x - s) * G;
    const float y = v + s;
    s = y + v;
    if (std::abs(s) < 1.0e-20f) // decaying state snaps at -400 dB, so silence leaves no subnormals
        s = 0.0f;
    return y;
}

DelayVoice::DelayVoice() noexcept
{
    lowCut_.reset(80.0f);
    highCut_.reset(5000.0f);
}

void DelayVoice::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    const double longest = kMaxMs * kWideRatio * 1e-3 * sampleRate;
    lineL_.prepare(static_cast<int>(std::ceil(longest)) + kChunk + 4);
    lineR_.prepare(static_cast<int>(std::ceil(longest)) + kChunk + 4);
    time_.prepare(sampleRate, 0.080);
    lowCut_.prepare(sampleRate, 0.020);
    highCut_.prepare(sampleRate, 0.020);
    feedback_.prepare(sampleRate, 0.020);
    mix_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    setTimeMs(timeMs_);
    reset();
}

void DelayVoice::reset() noexcept
{
    lineL_.reset();
    lineR_.reset();
    lowL_ = lowR_ = highL_ = highR_ = OnePole{};
    time_.reset(time_.target());
    lowCut_.reset(lowCut_.target());
    highCut_.reset(highCut_.target());
    feedback_.reset(feedback_.target());
    mix_.reset(mix_.target());
    enabled_.reset(enabled_.target());
    lowG_ = cutCoeff(lowCut_.current());
    setHighG(cutCoeff(highCut_.current()));
}

void DelayVoice::setTimeMs(float ms) noexcept
{
    timeMs_ = clampOrMin(ms, kMinMs, kMaxMs);
    time_.setTarget(static_cast<float>(timeMs_ * 1e-3 * sampleRate_));
}
void DelayVoice::setFeedbackPct(float pct) noexcept
{
    feedback_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void DelayVoice::setMixPct(float pct) noexcept
{
    mix_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void DelayVoice::setLowCutHz(float hz) noexcept
{
    lowCut_.setTarget(clampOrMin(hz, kMinLowCutHz, kMaxLowCutHz));
}
void DelayVoice::setHighCutHz(float hz) noexcept
{
    highCut_.setTarget(clampOrMin(hz, kMinHighCutHz, kMaxHighCutHz));
}

// G = g / (1 + g), g = tan(π f / fs): the prewarped TPT one-pole gain (Zavalishin §3.10).
float DelayVoice::cutCoeff(float hz) const noexcept
{
    if (sampleRate_ <= 0.0)
        return 0.0f;
    const double g =
        std::tan(3.14159265358979323846 * std::min(static_cast<double>(hz), 0.49 * sampleRate_) / sampleRate_);
    return static_cast<float>(g / (1.0 + g));
}

// The TPT one-pole low-pass g(1 + z⁻¹) / ((1 + g) − (1 − g) z⁻¹) has a DC group delay of 1/(2g) samples
// (0.5 from the zero, p/(1 − p) from the pole p = (1 − g)/(1 + g)); with G = g/(1 + g) that is (1 − G)/(2G).
void DelayVoice::setHighG(float G) noexcept
{
    highG_ = G;
    highDelay_ = G > 0.0f ? (1.0f - G) / (2.0f * G) : 0.0f;
}

void DelayVoice::runChunk(const float* inL, const float* inR, float* wetL, float* wetR, int n) noexcept
{
    float tapL[kChunk], tapR[kChunk], lowG[kChunk], highG[kChunk];
    const float rightRatio = stereo_ == Stereo::wide ? kWideRatio : 1.0f;
    // k samples of this chunk are not pushed yet when tap k is read, so read(d − k) is x[n0 + k − d]. The high cut's
    // DC group delay is taken off the read, so the centre of each repeat lands on the set time.
    for (int k = 0; k < n; ++k) {
        if (lowCut_.isSmoothing())
            lowG_ = cutCoeff(lowCut_.next());
        if (highCut_.isSmoothing())
            setHighG(cutCoeff(highCut_.next()));
        lowG[k] = lowG_;
        highG[k] = highG_;
        const float d = time_.next() - highDelay_;
        const float fk = static_cast<float>(k);
        tapL[k] = lineL_.read(d - fk);
        tapR[k] = lineR_.read(d * rightRatio - fk);
    }
    for (int k = 0; k < n; ++k) {
        const float g = kMaxLoopGain * feedback_.next();
        const float l = finiteOrZero(inL[k]), r = finiteOrZero(inR[k]);
        const float yL = highL_.lp(tapL[k] - lowL_.lp(tapL[k], lowG[k]), highG[k]);
        if (stereo_ == Stereo::mono) {
            const float push = 0.5f * (l + r) + g * limit(yL);
            lineL_.push(push);
            lineR_.push(push); // keeps both histories aligned for the block's voice copy on a stereo change
            wetL[k] = wetR[k] = yL;
            continue;
        }
        const float yR = highR_.lp(tapR[k] - lowR_.lp(tapR[k], lowG[k]), highG[k]);
        if (stereo_ == Stereo::pingpong) {
            lineL_.push(0.5f * (l + r) + g * limit(yR));
            lineR_.push(g * limit(yL));
        } else {
            lineL_.push(l + g * limit(yL));
            lineR_.push(r + g * limit(yR));
        }
        wetL[k] = yL;
        wetR[k] = yR;
    }
}

void DelayVoice::processWet(const float* inL, const float* inR, float* wetL, float* wetR, int numSamples) noexcept
{
    if (numSamples <= 0 || inL == nullptr || inR == nullptr || wetL == nullptr || wetR == nullptr)
        return;
    if (!isPrepared()) {
        std::fill(wetL, wetL + numSamples, 0.0f);
        std::fill(wetR, wetR + numSamples, 0.0f);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += kChunk) {
        const int n = std::min(kChunk, numSamples - pos);
        runChunk(inL + pos, inR + pos, wetL + pos, wetR + pos, n);
    }
}

void DelayVoice::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    float wl[kChunk], wr[kChunk];
    for (int pos = 0; pos < numSamples; pos += kChunk) {
        const int n = std::min(kChunk, numSamples - pos);
        runChunk(left + pos, right + pos, wl, wr, n);
        for (int k = 0; k < n; ++k) {
            const float g = mix_.next() * enabled_.next();
            left[pos + k] = finiteOrZero(left[pos + k]) + g * wl[k]; // g == 0: exactly the dry signal
            right[pos + k] = finiteOrZero(right[pos + k]) + g * wr[k];
        }
    }
}

} // namespace osp::dsp
