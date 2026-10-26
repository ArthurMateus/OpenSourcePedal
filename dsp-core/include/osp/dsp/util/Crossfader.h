#pragma once

// Constant-amplitude (linear) crossfade between two time-aligned signals: block enable/bypass and internal
// enum/bool switches (signal-flow.md §5, parameters.md §1.4). Pure C++20, RT-safe after prepare().
//
// out = (1 − g)·a + g·b, with g ramping linearly between 0 and 1. Linear, not equal-power, because both inputs are
// latency-aligned and strongly correlated, so the gains summing to 1 keep the level constant (signal-flow.md §5).

#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp {

class Crossfader {
public:
    // Block enable length. signal-flow.md §5 owns this value; parameters.md references it.
    static constexpr double kBlockEnableSeconds = 0.010;

    // Non-RT. Internal enum switches pass their own `xf N` length from parameters.md.
    void prepare(double sampleRate, double seconds = kBlockEnableSeconds) noexcept { ramp_.prepare(sampleRate, seconds); }

    // Jumps to a or b with no fade.
    void reset(bool towardsB) noexcept { ramp_.reset(towardsB ? 1.0f : 0.0f); }

    // Fades towards b (true) or a (false). A new target mid-fade reverses from the current position.
    void setTarget(bool towardsB) noexcept { ramp_.setTarget(towardsB ? 1.0f : 0.0f); }

    bool isFading() const noexcept { return ramp_.isRamping(); }
    bool target() const noexcept { return ramp_.target() > 0.5f; }
    // True when settled fully on `a`: a bypassed block may then skip computing `b`.
    bool isSettledOnA() const noexcept { return !ramp_.isRamping() && ramp_.current() == 0.0f; }
    float position() const noexcept { return ramp_.current(); }
    int lengthSamples() const noexcept { return ramp_.lengthSamples(); }

    // `out` may alias `a` or `b`. Safe for any n >= 0; works before prepare() (jumps instead of fading).
    // Once settled, the output is an exact copy of a or b and the other input is never read, so a bypassed block
    // may leave `b` stale (even NaN) while settled on `a`.
    void process(const float* a, const float* b, float* out, int numSamples) noexcept
    {
        int i = 0;
        for (; i < numSamples && ramp_.isRamping(); ++i) {
            const float g = ramp_.next();
            out[i] = (1.0f - g) * a[i] + g * b[i];
        }
        const float* const src = ramp_.current() == 0.0f ? a : b;
        if (src != out)
            for (; i < numSamples; ++i)
                out[i] = src[i];
    }

private:
    LinearRamp ramp_;
};

} // namespace osp::dsp
