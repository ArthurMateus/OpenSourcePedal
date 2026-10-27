#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// The saturation inside the delay loop for the analog (BBD) and tape voicings (issue #71 part 3/4). Per signal-flow §3
// and the ADR-0005 D2 note it runs at 2× at 44.1/48 kHz and 1× at 88.2/96 kHz, with 1st-order ADAA at that rate.
// The Oversampler is block-based, so DelayVoice reads a chunk of taps first and saturates the whole chunk; the
// up/down delay plus ADAA's half sample (delaySamples()) is taken off the delay line's read, so L_dly stays 0 and the
// echo time stays exact.
// Shape (own normalisation of a biased tanh; β = 0 for tape, β > 0 for the BBD's asymmetry):
//   f(x) = (tanh(g·x + β) − c) / g,   c = tanh(β),   so f(0) = 0 and f'(x) <= 1 everywhere (f' = 1 − c² at 0),
// with antiderivative F1(x) = (log cosh(g·x + β) / g − c·x) / g, and ADAA1 y = (F1(x) − F1(x₋₁)) / (x − x₋₁)
// (J. D. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time
// Convolution", DAFx-16, https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf, accessed
// 2026-10-27). Its slope never exceeds 1, so it can never raise the loop gain, however hard it is driven. (Normalising
// by the slope at 0 instead would give slopes up to cosh²(β) > 1 away from 0 and lets the BBD loop self-oscillate.)

#include "osp/dsp/oversampling/Oversampler.h"

namespace osp::dsp {

class LoopSaturator {
public:
    // Non-RT: allocates the oversampler for chunks of up to maxChunk samples.
    void prepare(double sampleRate, int maxChunk);
    // RT-safe.
    void reset() noexcept;
    // RT-safe. In place on n <= maxChunk base-rate samples. drive[k] is g for base sample k (> 0); bias is β.
    void process(float* x, const float* drive, float bias, int n) noexcept;
    // The up/down delay (measured at 1 kHz, ADR-0005 D4) plus ADAA's half sample, in base-rate samples.
    double delaySamples() const noexcept { return delay_; }
    int factor() const noexcept { return os_.getFactor(); }

private:
    double f(double x) const noexcept;
    double F1(double x) const noexcept;

    Oversampler os_;
    double delay_ = 0.0;
    double x1_ = 0.0, f1x1_ = 0.0;
    double g1_ = 0.0, b1_ = 0.0, c_ = 0.0; // the current shape: g, β, tanh(β)
};

} // namespace osp::dsp
