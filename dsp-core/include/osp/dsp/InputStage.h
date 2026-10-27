#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Input block, `input.gainDb` in parameters.md §2.1 (Global-Lock): mono trim, then a DC blocker (signal-flow §4,
// L_in = 0, minimum phase). The tuner tap reads after this block (signal-flow §6).
// - Trim: -24...+24 dB, a 20 ms linear ramp of the linear gain (Gain, parameters.md §1.4). NaN maps to the range
//   minimum, as in the other blocks.
// - DC blocker: the one-pole/one-zero high pass y[n] = x[n] - x[n-1] + R y[n-1], R = exp(-2 pi fc / fs), fc =
//   kDcCornerHz (J. O. Smith, "Introduction to Digital Filters", DC Blocker section, CCRMA, accessed 2026-10-26). At
//   5 Hz it is -0.016 dB at 82 Hz (low E) and below 0.01 dB from 110 Hz up.
// - Non-finite input samples count as 0, so a NaN never stays in the filter state (RT-9 is the output's guard).

#include "osp/dsp/Gain.h"

namespace osp::dsp {

// The DC blocker on its own (public so tests can build a reference through the identical filter).
struct DcBlocker {
    float r = 0.0f, x1 = 0.0f, y1 = 0.0f;
    void prepare(double sampleRate, double cornerHz) noexcept;
    void reset() noexcept { x1 = y1 = 0.0f; }
    float tick(float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = (y > -1.0e-20f && y < 1.0e-20f) ? 0.0f : y; // no subnormals in silence
        return y;
    }
};

class InputStage {
public:
    static constexpr float kMinGainDb = -24.0f, kMaxGainDb = 24.0f;
    static constexpr double kDcCornerHz = 5.0;

    // Non-RT. No allocation.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears the DC blocker and jumps the trim to its target: two renders after reset() match.
    void reset() noexcept;
    // RT-safe, once per block. Clamped to -24...+24 dB; NaN gives -24.
    void setGainDb(float dB) noexcept;
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. in == out is allowed. Before prepare() it zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return trim_.isPrepared(); }

private:
    Gain trim_;
    DcBlocker dc_;
    bool bypassed_ = false;
};

} // namespace osp::dsp
