#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// One drive slot, `driveN.*` in parameters.md §2.4. It runs at the OS-1 oversampled rate: the shared OS-1 region
// (#67 part 2) owns the Oversampler and feeds both slots (ADR-0005 D2, signal-flow §3). Mix and enable take their dry
// signal inside that domain, so dry and wet share one up/down chain (ADR-0005 D4).
//
// Per sample: tight HPF (2nd-order TPT SVF, lowCutHz) -> voicing -> tone (high shelf) -> bright -> level -> mix.
// Voicing = pre-HPF (1st order) -> gain -> ADAA shaper -> post-LPF (1st order) -> 10 Hz DC blocker:
//   u = G·hp(x) + c·x, y = kOut·f(u). The table in Drive.cpp holds G (dB over drive 0…10), the corners, c and kOut.
// - mid: TS-style. 720 Hz pre-HPF with the full-band path added (c = 1), so only the mids get the gain and clip:
//   the HPF that the 0.047 µF + 4.7 kΩ leg forms in that circuit (tone-targets-b F1). tanh, ADAA1.
// - transparent: 0.35·u + 0.65·tanh(u) (own design), ADAA1. Clean boost with edge; kOut = 1.
// - dist: 160 Hz pre-HPF, hard clip, ADAA2. fuzz: 100 Hz pre-HPF, asymmetric hard clip (rails −1.3/+0.7), ADAA2.
// ADAA order per ADR-0005 D2 (soft clip 1st, hard clip/fuzz 2nd). Every corner, gain range and the shapes are
// engineering starting points (unverified), to be calibrated by the preset designer against renders.
//
// Latency: kDelaySamples (1 sample at the rate it runs at) for every voicing and when disabled, so a type change or
// enable never moves the region latency (REQ-303). ADAA2 voicings delay by exactly 1 sample, ADAA1 voicings by 0.5;
// that 0.5-sample lead is about 0.3 rad at 20 kHz with 4x at 48 kHz, and only matters inside the 10 ms type fade.
// The mix and enable dry paths are delayed by 1 sample.

#include "osp/dsp/oversampling/Adaa.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

#include <array>

namespace osp::dsp {

enum class DriveType { mid, transparent, dist, fuzz };

namespace detail {
// 0.35·u + 0.65·tanh(u): F1 = 0.35·u²/2 + 0.65·log cosh u (TanhShape::F1 is the overflow-safe log cosh).
struct SoftLinearShape {
    static constexpr double kLinear = 0.35;
    static double f(double x) noexcept { return kLinear * x + (1.0 - kLinear) * TanhShape::f(x); }
    static double F1(double x) noexcept { return 0.5 * kLinear * x * x + (1.0 - kLinear) * TanhShape::F1(x); }
};

// Hard clip shifted by a bias b and re-centred, f(u) = clip(u + b) − b: rails −1 − b and 1 − b, f(0) = 0.
// F1(u) = H1(u + b) − b·u and F2(u) = H2(u + b) − b·u²/2, with H1, H2 the hard clip antiderivatives.
struct AsymClipShape {
    static constexpr double kBias = 0.3;
    static double f(double x) noexcept { return HardClipShape::f(x + kBias) - kBias; }
    static double F1(double x) noexcept { return HardClipShape::F1(x + kBias) - kBias * x; }
    static double F2(double x) noexcept { return HardClipShape::F2(x + kBias) - 0.5 * kBias * x * x; }
};

// 1st-order TPT one-pole (Zavalishin, "The Art of VA Filter Design", 2018, §3.10). g = tan(pi f / fs) / (1 + ...).
struct OnePoleTpt {
    float g = 0.0f, s = 0.0f;
    float lowPass(float x) noexcept
    {
        const float v = (x - s) * g;
        const float y = v + s;
        s = y + v;
        if (s > -1.0e-20f && s < 1.0e-20f)
            s = 0.0f;
        return y;
    }
    float highPass(float x) noexcept { return x - lowPass(x); }
};
} // namespace detail

class DriveStage {
public:
    static constexpr double kDelaySamples = 1.0; // at the rate it runs at, for every voicing and when disabled

    DriveStage() noexcept;

    // Non-RT, no allocation. sampleRate is the OS-1 oversampled rate.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears every state and jumps every smoother to its target: two renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Values are clamped to the parameters.md ranges.
    void setType(DriveType type) noexcept;
    void setDrive(float knob) noexcept;
    void setTone(float knob) noexcept;
    void setLevelDb(float dB) noexcept;
    void setLowCutHz(float hz) noexcept;
    void setBright(bool on) noexcept;
    void setMixPct(float pct) noexcept;
    void setEnabled(bool on) noexcept; // 10 ms linear crossfade with the delayed dry signal (signal-flow §5)
    // Hard bypass for tests (signal-flow §5): output = input delayed by kDelaySamples.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe, in place, at the oversampled rate. Before prepare() it zero-fills.
    void process(float* io, int numSamples) noexcept;

    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

private:
    struct Path {
        detail::OnePoleTpt pre, post, dc;
        Adaa1<TanhShape> tanh;
        Adaa1<detail::SoftLinearShape> soft;
        Adaa2<HardClipShape> hard;
        Adaa2<detail::AsymClipShape> asym;
        float gain = 1.0f, gainKnob = -1.0f; // linear gain cached for gainKnob
    };

    float voicing(DriveType type, float x, float knob) noexcept;
    void resetPath(DriveType type) noexcept;
    void updateLowCut(float hz) noexcept;
    void updateTone(float knob) noexcept;

    double sampleRate_ = 0.0;
    DriveType typeFrom_ = DriveType::mid, typeTo_ = DriveType::mid, pendingType_ = DriveType::mid;
    bool bypassed_ = false, enabled_ = true;
    float dry1_ = 0.0f; // 1-sample dry delay
    std::array<Path, 4> paths_{};
    SvfState lowCutState_, toneState_, brightState_;
    SvfCoeffs lowCutCoeffs_, toneCoeffs_, brightCoeffs_;
    OnePoleSmoother drive_;  // 30 ms, knob
    OnePoleSmoother tone_;   // 20 ms, knob
    OnePoleSmoother lowCut_; // 20 ms, Hz
    LinearRamp level_;       // 20 ms, linear gain
    LinearRamp mix_;         // 20 ms, 0…1
    LinearRamp typeFade_;    // xf 10, 0 = typeFrom_, 1 = typeTo_
    LinearRamp bright_;      // xf 10, 0…1
    LinearRamp enable_;      // 10 ms block enable, 0…1
};

} // namespace osp::dsp
