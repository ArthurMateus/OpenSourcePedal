#include "osp/dsp/Gate.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {
namespace {

constexpr double kDetectorSeconds = 0.010;
constexpr double kParamSmoothingSeconds = 0.020; // parameters.md §2.2: threshold and range, 20 ms
// Fixed close debounce before the hold starts: one period of about 40 Hz, below the low B of a 7-string (61.7 Hz).
// Without it, the peak envelope dips between the half-cycle peaks of a low note held just above threshold, and with
// hold 0 and hysteresis <= 3 dB the gate closed and reopened every half-cycle (PR #88 review, 494 transitions/1.5 s).
constexpr double kCloseDebounceSeconds = 0.025;

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

// NaN-safe clamp: a NaN host value maps to `lo` instead of propagating (std::clamp passes NaN through).
float clampSafe(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

float coeffFor(double seconds, double sampleRate) noexcept
{
    return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
}

} // namespace

void Gate::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    scratch_.assign(static_cast<std::size_t>(std::max(maxBlockSize, 1)), 0.0f);
    thresholdLin_.prepare(sampleRate, kParamSmoothingSeconds);
    rangeLin_.prepare(sampleRate, kParamSmoothingSeconds);
    enable_.prepare(sampleRate);
    detectorCoeff_ = coeffFor(kDetectorSeconds, sampleRate);
    setParameters(params_);
    reset();
}

void Gate::reset() noexcept
{
    thresholdLin_.reset(dbToLin(params_.thresholdDb));
    rangeLin_.reset(dbToLin(params_.rangeDb));
    enable_.reset(enable_.target());
    envelope_ = 0.0f;
    target_ = rangeLin_.current();
    deviation_ = 0.0f;
    closeDelayLeft_ = 0;
    open_ = false;
}

void Gate::setParameters(const Params& p) noexcept
{
    params_.thresholdDb = clampSafe(p.thresholdDb, -90.0f, -20.0f);
    params_.hysteresisDb = clampSafe(p.hysteresisDb, 0.0f, 10.0f);
    params_.attackMs = clampSafe(p.attackMs, 0.1f, 10.0f);
    params_.holdMs = clampSafe(p.holdMs, 0.0f, 200.0f);
    params_.releaseMs = clampSafe(p.releaseMs, 10.0f, 500.0f);
    params_.rangeDb = clampSafe(p.rangeDb, -90.0f, 0.0f);
    params_.detector = p.detector;

    thresholdLin_.setTarget(dbToLin(params_.thresholdDb));
    rangeLin_.setTarget(dbToLin(params_.rangeDb));
    closeRatio_ = dbToLin(-params_.hysteresisDb);
    if (sampleRate_ > 0.0) {
        attackCoeff_ = coeffFor(params_.attackMs * 1e-3, sampleRate_);
        releaseCoeff_ = coeffFor(params_.releaseMs * 1e-3, sampleRate_);
        closeDelaySamples_ =
            static_cast<int>(std::lround((kCloseDebounceSeconds + params_.holdMs * 1e-3) * sampleRate_));
    }
}

void Gate::setEnabled(bool enabled) noexcept
{
    enable_.setTarget(enabled);
}

void Gate::computeGain(const float* key, float* gain, int numSamples) noexcept
{
    const bool rms = params_.detector == Detector::Rms;
    for (int i = 0; i < numSamples; ++i) {
        const float x = key[i];
        float level;
        if (rms) {
            envelope_ = x * x + detectorCoeff_ * (envelope_ - x * x);
            level = envelope_; // compared against the squared thresholds
        } else {
            envelope_ = std::max(std::abs(x), envelope_ * detectorCoeff_);
            level = envelope_;
        }

        const float threshold = thresholdLin_.next();
        const float open = rms ? threshold * threshold : threshold;
        const float close = rms ? open * closeRatio_ * closeRatio_ : open * closeRatio_;
        if (!open_) {
            if (level >= open) {
                open_ = true;
                closeDelayLeft_ = closeDelaySamples_;
            }
        } else if (level >= close) {
            closeDelayLeft_ = closeDelaySamples_;
        } else if (closeDelayLeft_ > 0) {
            --closeDelayLeft_;
        } else {
            open_ = false;
        }

        const float floor = rangeLin_.next();
        const float target = open_ ? 1.0f : floor;
        if (target != target_) {
            deviation_ += target_ - target; // keep gain = target_ + deviation_ continuous
            target_ = target;
        }
        deviation_ *= deviation_ < 0.0f ? attackCoeff_ : releaseCoeff_; // below target: rising, attack τ
        if (std::abs(deviation_) <= 1.0e-6f * target_)
            deviation_ = 0.0f;
        gain[i] = target_ + deviation_;
    }
}

void Gate::applyGain(const float* gain, float* io, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        io[i] *= gain[i];
}

void Gate::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (scratch_.empty()) {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }
    const int chunk = static_cast<int>(scratch_.size());
    for (int pos = 0; pos < numSamples; pos += chunk) {
        const int n = std::min(chunk, numSamples - pos);
        float* const wet = scratch_.data();
        computeGain(in + pos, wet, n);
        for (int i = 0; i < n; ++i)
            wet[i] *= in[pos + i];
        enable_.process(in + pos, wet, out + pos, n);
    }
}

} // namespace osp::dsp
