#include "osp/dsp/Compressor.h"

#include <cmath>
#include <initializer_list>

namespace osp::dsp {

namespace {

// Maps NaN to lo, so a broken host value can never poison the detector.
float clampf(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

float dbToGain(float dB) noexcept
{
    return std::exp(dB * 0.115129255f); // ln(10) / 20
}

float coefficient(double sampleRate, float ms) noexcept
{
    return static_cast<float>(std::exp(-1.0 / (0.001 * static_cast<double>(ms) * sampleRate)));
}

} // namespace

float Compressor::staticGainReductionDb(float xDb, float thresholdDb, float ratio, float kneeDb) noexcept
{
    const float d = xDb - thresholdDb;
    const float slope = 1.0f - 1.0f / ratio;
    if (2.0f * d <= -kneeDb)
        return 0.0f;
    if (2.0f * d < kneeDb) {
        const float t = d + 0.5f * kneeDb;
        return slope * t * t / (2.0f * kneeDb);
    }
    return slope * d;
}

float Compressor::Detector::next(float x) noexcept
{
    const float a = x > y ? attack : release;
    y = a * y + (1.0f - a) * x;
    if (y < 1.0e-6f) // ends the release tail exactly instead of decaying into subnormals
        y = 0.0f;
    return y;
}

Compressor::Compressor() noexcept
{
    threshold_.reset(thresholdTarget_);
    blend_.reset(1.0f);
    makeup_.reset(1.0f);
    updateAutoMakeup();
}

void Compressor::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    threshold_.prepare(sampleRate, 0.020);
    blend_.prepare(sampleRate, 0.020);
    makeup_.prepare(sampleRate, 0.020);
    autoGain_.prepare(sampleRate, 0.010);
    modeFade_.prepare(sampleRate, 0.010);
    updateCoefficients();
    reset();
}

void Compressor::reset() noexcept
{
    for (Detector* d : {&vca_, &optoFast_, &optoSlow_, &dual1_, &dual2_})
        d->y = 0.0f;
    threshold_.reset(thresholdTarget_);
    blend_.reset(blend_.target());
    makeup_.reset(makeup_.target());
    autoGain_.reset(autoGain_.target());
    modeFrom_ = modeTo_ = pendingMode_;
    modeFade_.reset(0.0f);
    grDb_ = 0.0f;
}

void Compressor::setThresholdDb(float dB) noexcept
{
    thresholdTarget_ = clampf(dB, -50.0f, 0.0f);
    threshold_.setTarget(thresholdTarget_);
    updateAutoMakeup();
}

void Compressor::setRatio(float ratio) noexcept
{
    ratio_ = clampf(ratio, 1.0f, 20.0f);
    updateAutoMakeup();
}

void Compressor::setKneeDb(float dB) noexcept
{
    kneeDb_ = clampf(dB, 0.0f, 12.0f);
    updateAutoMakeup();
}

void Compressor::setAttackMs(float ms) noexcept
{
    const float v = clampf(ms, 0.1f, 100.0f);
    if (v != attackMs_) {
        attackMs_ = v;
        updateCoefficients();
    }
}

void Compressor::setReleaseMs(float ms) noexcept
{
    const float v = clampf(ms, 20.0f, 1000.0f);
    if (v != releaseMs_) {
        releaseMs_ = v;
        updateCoefficients();
    }
}

void Compressor::setBlendPct(float pct) noexcept
{
    blend_.setTarget(0.01f * clampf(pct, 0.0f, 100.0f));
}

void Compressor::setMakeupDb(float dB) noexcept
{
    makeupDb_ = clampf(dB, 0.0f, 24.0f);
    makeup_.setTarget(dbToGain(makeupDb_));
}

void Compressor::setAutoMakeup(bool on) noexcept
{
    autoMakeup_ = on;
    updateAutoMakeup();
}

void Compressor::setMode(CompMode mode) noexcept
{
    pendingMode_ = mode;
    if (!isPrepared())
        modeFrom_ = modeTo_ = mode;
}

void Compressor::updateCoefficients() noexcept
{
    if (!isPrepared())
        return;
    const float a = coefficient(sampleRate_, attackMs_), r = coefficient(sampleRate_, releaseMs_);
    vca_.attack = optoFast_.attack = dual1_.attack = a;
    vca_.release = optoFast_.release = dual1_.release = dual2_.release = r;
    optoSlow_.attack = coefficient(sampleRate_, kOptoSlowAttack * attackMs_);
    optoSlow_.release = coefficient(sampleRate_, kOptoSlowRelease * releaseMs_);
    dual2_.attack = coefficient(sampleRate_, kDualStage2Attack * attackMs_);
}

void Compressor::updateAutoMakeup() noexcept
{
    const float dB = autoMakeup_ ? staticGainReductionDb(kAutoMakeupRefDb, thresholdTarget_, ratio_, kneeDb_) : 0.0f;
    autoGain_.setTarget(dbToGain(dB));
}

void Compressor::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        for (int i = 0; i < numSamples; ++i)
            out[i] = 0.0f;
        return;
    }
    if (bypassed_) {
        for (int i = 0; i < numSamples; ++i)
            out[i] = in[i];
        return;
    }

    // Mode switches start at a block boundary, one at a time (xf 10: old and new run in parallel).
    if (!modeFade_.isRamping()) {
        if (modeFade_.current() > 0.0f)
            modeFrom_ = modeTo_;
        modeFade_.reset(0.0f);
        if (pendingMode_ != modeTo_) {
            modeTo_ = pendingMode_;
            modeFade_.setTarget(1.0f);
        }
    }

    const float rootRatio = std::sqrt(ratio_);
    float gr[3] = {};
    for (int i = 0; i < numSamples; ++i) {
        const float x = in[i];
        const float thr = threshold_.next();
        const float xDb = std::fmin(std::fmax(20.0f * std::log10(std::fmax(std::abs(x), 1.0e-6f)), -120.0f), 60.0f);

        // Every mode runs every sample, so a mode switch fades between two warm detectors.
        const float s = staticGainReductionDb(xDb, thr, ratio_, kneeDb_);
        gr[static_cast<int>(CompMode::vca)] = vca_.next(s);
        gr[static_cast<int>(CompMode::opto)] = std::fmax(optoFast_.next(s), optoSlow_.next(kOptoSlowDepth * s));
        const float g1 = dual1_.next(staticGainReductionDb(xDb, thr, rootRatio, kneeDb_));
        gr[static_cast<int>(CompMode::dual)] =
            g1 + dual2_.next(staticGainReductionDb(xDb - g1, thr, rootRatio, kneeDb_));

        const float p = modeFade_.next();
        const float grTo = gr[static_cast<int>(modeTo_)];
        float g = dbToGain(-grTo);
        if (p < 1.0f && modeFrom_ != modeTo_)
            g = (1.0f - p) * dbToGain(-gr[static_cast<int>(modeFrom_)]) + p * g;
        grDb_ = grTo;

        const float wet = x * g * makeup_.next() * autoGain_.next();
        const float b = blend_.next();
        out[i] = (1.0f - b) * x + b * wet;
    }
}

} // namespace osp::dsp
