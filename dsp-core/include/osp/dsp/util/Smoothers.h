#pragma once

// Parameter smoothing primitives used by every dsp-core block (parameters.md §1.4, realtime-rules.md RT-13).
// Pure C++20: no JUCE, no OS headers. All members are RT-safe except prepare(), which only computes coefficients
// (no allocation either). Before prepare() every setTarget() snaps, so a block that is configured before it is
// prepared starts at its target values.
//
// Sources: linear ramp of the linear gain per sample (parameters.md §1.4, "Gain / level"); one-pole exponential
// smoother y[n] = t + a·(y[n−1] − t), a = exp(−1 / (τ·fs)), so τ is the 0→63 % time (J. O. Smith, "Introduction
// to Digital Filters", one-pole lowpass, https://ccrma.stanford.edu/~jos/filters/, accessed 2026-10-25).

#include <cmath>

namespace osp::dsp {

// Linear ramp to the latest target over a fixed length. Each value is computed from the remaining count, not
// accumulated, so the ramp lands exactly on the target and never drifts.
class LinearRamp {
public:
    // Non-RT. Ramp length = round(fs · seconds), at least 1 sample.
    void prepare(double sampleRate, double seconds) noexcept
    {
        const auto samples = static_cast<int>(std::lround(sampleRate * seconds));
        length_ = samples > 0 ? samples : 1;
        reset(target_);
    }

    void reset(float value) noexcept
    {
        current_ = target_ = value;
        step_ = 0.0f;
        remaining_ = 0;
    }

    // Starts a new ramp from the current value. A new target mid-ramp restarts the ramp, so there is no jump.
    void setTarget(float target) noexcept
    {
        if (target == target_)
            return;
        target_ = target;
        if (length_ == 0) {
            current_ = target_;
            return;
        }
        remaining_ = length_;
        step_ = (target_ - current_) / static_cast<float>(length_);
    }

    float next() noexcept
    {
        if (remaining_ > 0) {
            --remaining_;
            current_ = target_ - step_ * static_cast<float>(remaining_);
        }
        return current_;
    }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }
    bool isRamping() const noexcept { return remaining_ > 0; }
    int lengthSamples() const noexcept { return length_; }
    bool isPrepared() const noexcept { return length_ > 0; }

private:
    int length_ = 0;
    int remaining_ = 0;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
};

// One-pole exponential smoother. It stores the deviation from the target, which decays geometrically, so it
// never stalls short of the target in float precision. It snaps once the deviation is below a relative
// threshold, so it settles exactly (no endless tail of tiny steps, no subnormals).
class OnePoleSmoother {
public:
    // Non-RT. tauSeconds <= 0 means "no smoothing" (every target is applied immediately).
    void prepare(double sampleRate, double tauSeconds) noexcept
    {
        coeff_ = tauSeconds > 0.0 ? static_cast<float>(std::exp(-1.0 / (tauSeconds * sampleRate))) : 0.0f;
        prepared_ = true;
        reset(target_);
    }

    void reset(float value) noexcept
    {
        target_ = value;
        deviation_ = 0.0f;
    }

    void setTarget(float target) noexcept
    {
        deviation_ = prepared_ ? current() - target : 0.0f;
        target_ = target;
    }

    float next() noexcept
    {
        deviation_ *= coeff_;
        if (std::abs(deviation_) <= kSnap * (1.0f + std::abs(target_)))
            deviation_ = 0.0f;
        return current();
    }

    float current() const noexcept { return target_ + deviation_; }
    float target() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return deviation_ != 0.0f; }

    // Relative snap distance: 1e-6 is about -120 dB of the parameter's magnitude.
    static constexpr float kSnap = 1.0e-6f;

private:
    float coeff_ = 0.0f;
    float target_ = 0.0f;
    float deviation_ = 0.0f;
    bool prepared_ = false;
};

} // namespace osp::dsp
