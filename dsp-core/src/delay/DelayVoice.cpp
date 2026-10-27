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
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kBbdLfoHz = 0.7, kWowHz = 0.8, kFlutterHz = 6.5;
constexpr float kFlutterMs = 0.03f;

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
    saturation_.reset(0.2f);
}

void DelayVoice::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    const double longest = kMaxMs * kWideRatio * 1e-3 * sampleRate;
    const int size = static_cast<int>(std::ceil(longest + 1.5 * kMaxModMs * 1e-3 * sampleRate)) + kChunk + 4;
    lineL_.prepare(size);
    lineR_.prepare(size);
    satL_.prepare(sampleRate, kChunk);
    satR_.prepare(sampleRate, kChunk);
    bump_ = svfDesign(SvfType::bell, sampleRate, 80.0, 1.2, 3.0);
    time_.prepare(sampleRate, 0.080);
    lowCut_.prepare(sampleRate, 0.020);
    highCut_.prepare(sampleRate, 0.020);
    feedback_.prepare(sampleRate, 0.020);
    mix_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    modDepth_.prepare(sampleRate, 0.020);
    saturation_.prepare(sampleRate, 0.020);
    setTimeMs(timeMs_);
    reset();
}

void DelayVoice::reset() noexcept
{
    lineL_.reset();
    lineR_.reset();
    lowL_ = lowR_ = highL_ = highR_ = OnePole{};
    satL_.reset();
    satR_.reset();
    colourL_ = colourR_ = bumpL_ = bumpR_ = SvfState{};
    colourHz_ = colourDelay_ = 0.0f;
    lfoPhase_ = flutterPhase_ = 0.0;
    modDepth_.reset(modDepth_.target());
    saturation_.reset(saturation_.target());
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
void DelayVoice::setModDepthPct(float pct) noexcept
{
    modDepth_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
}
void DelayVoice::setSaturationPct(float pct) noexcept
{
    saturation_.setTarget(clampOrMin(pct, 0.0f, 100.0f) / 100.0f);
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

// Read-pointer modulation in samples for the base delay d: the BBD clock's triangle, or tape wow + flutter.
float DelayVoice::modOffset(float d) noexcept
{
    const double fs = sampleRate_;
    lfoPhase_ += (voicing_ == Voicing::tape ? kWowHz : kBbdLfoHz) / fs;
    lfoPhase_ -= std::floor(lfoPhase_);
    flutterPhase_ += kFlutterHz / fs;
    flutterPhase_ -= std::floor(flutterPhase_);
    const float depth = modDepth_.next();
    if (voicing_ == Voicing::analog) {
        const float tri = static_cast<float>(4.0 * std::abs(lfoPhase_ - 0.5) - 1.0);
        return depth * std::min(0.005f * d, static_cast<float>(1.5e-3 * fs)) * tri;
    }
    if (voicing_ == Voicing::tape) {
        const float wow = std::min(0.004f * d, static_cast<float>(kMaxModMs * 1e-3 * fs));
        return depth * (wow * static_cast<float>(std::sin(kTwoPi * lfoPhase_)) +
                        static_cast<float>(kFlutterMs * 1e-3 * fs * std::sin(kTwoPi * flutterPhase_)));
    }
    return 0.0f;
}

void DelayVoice::runChunk(const float* inL, const float* inR, float* wetL, float* wetR, int n) noexcept
{
    float tapL[kChunk], tapR[kChunk], lowG[kChunk], highG[kChunk], drive[kChunk];
    SvfCoeffs colour[kChunk];
    const bool coloured = voicing_ != Voicing::digital, two = stereo_ != Stereo::mono;
    const float rightRatio = stereo_ == Stereo::wide ? kWideRatio : 1.0f;
    // The saturator's up/down delay is taken off the read too (signal-flow §3: L_dly stays 0).
    const float loopDelay = coloured ? static_cast<float>(satL_.delaySamples()) : 0.0f;
    // k samples of this chunk are not pushed yet when tap k is read, so read(d − k) is x[n0 + k − d]. The high cut's
    // DC group delay is taken off the read, so the centre of each repeat lands on the set time.
    for (int k = 0; k < n; ++k) {
        if (lowCut_.isSmoothing())
            lowG_ = cutCoeff(lowCut_.next());
        if (highCut_.isSmoothing())
            setHighG(cutCoeff(highCut_.next()));
        lowG[k] = lowG_;
        highG[k] = highG_;
        const float t = time_.next();
        const float s = saturation_.next();
        drive[k] = 0.05f + 3.95f * s * s;
        if (coloured) {
            const float hz = voicing_ == Voicing::tape
                                 ? kTapeLowPassHz
                                 : std::clamp(0.45f * kBbdStages * static_cast<float>(sampleRate_) / (2.0f * t),
                                              kBbdMinHz, kBbdMaxHz);
            if (hz != colourHz_) {
                colourHz_ = hz;
                colour_ = svfDesign(SvfType::lowPass, sampleRate_, hz, 0.70710678, 0.0);
                // The 2nd-order Butterworth group delay at 1 kHz (ADR-0005 D4's reference frequency), bilinear-mapped:
                // τ = √2/(2g) · (1 + r²)/(1 + r⁴) samples, g = tan(π fc/fs), r = tan(π 1000/fs)/g.
                const double g = std::tan(3.14159265358979 * hz / sampleRate_);
                const double r = std::tan(3.14159265358979 * 1000.0 / sampleRate_) / g;
                colourDelay_ = static_cast<float>(1.41421356 / (2.0 * g) * (1.0 + r * r) / (1.0 + r * r * r * r));
            }
            colour[k] = colour_;
        }
        const float d = t + modOffset(t) - highDelay_ - (coloured ? loopDelay + colourDelay_ : 0.0f);
        const float fk = static_cast<float>(k);
        tapL[k] = lineL_.read(d - fk);
        tapR[k] = lineR_.read(d * rightRatio - fk);
    }
    if (coloured) {
        const float bias = voicing_ == Voicing::analog ? kBbdBias : 0.0f;
        for (int k = 0; k < n; ++k)
            tapL[k] = colourL_.tick(colour[k], tapL[k]);
        satL_.process(tapL, drive, bias, n);
        if (two) {
            for (int k = 0; k < n; ++k)
                tapR[k] = colourR_.tick(colour[k], tapR[k]);
            satR_.process(tapR, drive, bias, n);
        }
    }
    const bool tape = voicing_ == Voicing::tape;
    for (int k = 0; k < n; ++k) {
        const float g = kMaxLoopGain * feedback_.next();
        const float l = finiteOrZero(inL[k]), r = finiteOrZero(inR[k]);
        const float yL = highL_.lp(tapL[k] - lowL_.lp(tapL[k], lowG[k]), highG[k]);
        const float outL = tape ? bumpL_.tick(bump_, yL) : yL; // the head bump colours the wet, not the loop
        if (!two) {
            const float push = 0.5f * (l + r) + g * limit(yL);
            lineL_.push(push);
            lineR_.push(push); // keeps both histories aligned for the block's voice copy on a stereo change
            wetL[k] = wetR[k] = outL;
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
        wetL[k] = outL;
        wetR[k] = tape ? bumpR_.tick(bump_, yR) : yR;
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
