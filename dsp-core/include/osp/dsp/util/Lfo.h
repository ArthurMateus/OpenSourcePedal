#pragma once

// Shared modulation primitives for the `mod` block (parameters.md §3.3, issue #70). Pure C++20, header-only.
// - Lfo: a phasor in cycles. The rate is a one-pole smoothed target (20 ms, parameters.md §1.4 "filter frequency"
//   class), and the phase is accumulated in double, so the rate is exact to the sample clock and never drifts.
//   Voices read it at their own phase offsets with sine().
// - ModDelayLine: a power-of-two ring with a 4-point cubic (Catmull-Rom) fractional read, the interpolation that
//   time-based-effects.md §1.4 recommends for continuously modulated taps (P. Bourke, "Interpolation methods", 1999,
//   https://paulbourke.net/miscellaneous/interpolation/, accessed 2026-10-27). Same read as the doubler (#126); the
//   two should move to the delay PR's shared FractionalDelay (#116) once that is on main.
// prepare() allocates; every other member is RT-safe.

#include "osp/dsp/util/Smoothers.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osp::dsp {

class Lfo {
public:
    static constexpr double kTwoPi = 6.28318530717958647692;

    void prepare(double sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        rate_.prepare(sampleRate, 0.020);
    }
    // Phase in cycles; the rate jumps to its target.
    void reset(double phase = 0.0) noexcept
    {
        phase_ = phase - std::floor(phase);
        rate_.reset(rate_.target());
    }
    void setRateHz(float hz) noexcept { rate_.setTarget(hz); }
    float rateHz() const noexcept { return rate_.current(); }

    // The phase for this sample in [0, 1), then advances by one sample.
    double next() noexcept
    {
        const double p = phase_;
        phase_ += rate_.next() / sampleRate_;
        phase_ -= std::floor(phase_);
        return p;
    }

    static float sine(double phase) noexcept { return static_cast<float>(std::sin(kTwoPi * phase)); }

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    OnePoleSmoother rate_;
};

class ModDelayLine {
public:
    // Non-RT. Room for reads up to maxDelaySamples plus the interpolation margin.
    void prepare(double maxDelaySamples)
    {
        std::size_t size = 4;
        while (static_cast<double>(size) < maxDelaySamples + 4.0)
            size <<= 1;
        ring_.assign(size, 0.0f);
        mask_ = static_cast<unsigned>(size - 1);
        write_ = 0;
    }
    void reset() noexcept
    {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        write_ = 0;
    }
    void push(float x) noexcept { ring_[++write_ & mask_] = x; }

    // Delay in samples from the newest pushed sample, clamped to [1, size - 3] so all four points are history.
    float read(double delaySamples) const noexcept
    {
        const double d = std::clamp(delaySamples, 1.0, static_cast<double>(mask_ - 2));
        const auto i = static_cast<unsigned>(d);
        const auto t = static_cast<float>(d - i);
        const float p0 = ring_[(write_ - i + 1) & mask_], p1 = ring_[(write_ - i) & mask_],
                    p2 = ring_[(write_ - i - 1) & mask_], p3 = ring_[(write_ - i - 2) & mask_];
        return p1 + 0.5f * t *
                        ((p2 - p0) + t * ((2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) + t * (3.0f * (p1 - p2) + p3 - p0)));
    }
    bool isPrepared() const noexcept { return !ring_.empty(); }

private:
    std::vector<float> ring_;
    unsigned mask_ = 0, write_ = 0;
};

} // namespace osp::dsp
