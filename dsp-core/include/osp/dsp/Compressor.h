#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Feed-forward compressor, `comp.*` in parameters.md §2.3. Latency 0 (signal-flow §4: no look-ahead in v1).
//
// Algorithm: log-domain gain computer with a quadratic soft knee and a smooth branching peak detector applied
// to the gain reduction, as in D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor
// Design - A Tutorial and Analysis", J. Audio Eng. Soc. 60(6), 2012 (eqs. 4 and 17). Attack/release are the
// 0→63 % time constants of that detector (parameters.md §1.4).
//
// Modes (own design; the constants are not matched to any hardware, unverified):
// - vca:  one detector.
// - opto: program-dependent two-stage release. A fast detector (attack, release) runs in parallel with a slow
// one
//         that tracks kOptoSlowDepth of the gain reduction with kOptoSlowAttack x attack and kOptoSlowRelease
//         x release. GR = max(fast, slow): short peaks release fast; sustained compression charges the slow
//         stage, so the release drops quickly to the slow level and then tails off.
// - dual: two stages in series, each with ratio sqrt(R) and the same threshold, so the hard-knee static curve
//         is exactly R. The second stage sees the first stage's output and has 4x the attack.
//
// Auto makeup = the static gain reduction at the DI nominal peak level kAutoMakeupRefDb (tone-targets-a
// §3.4), so a DI at nominal level leaves at the same peak. It adds to the manual makeup. Makeup applies to
// the wet path.

#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp
{

enum class CompMode
{
    opto,
    vca,
    dual
};

class Compressor
{
public:
    static constexpr float kAutoMakeupRefDb = -12.0f;
    static constexpr float kOptoSlowDepth = 0.5f;
    static constexpr float kOptoSlowAttack = 20.0f;
    static constexpr float kOptoSlowRelease = 8.0f;
    static constexpr float kDualStage2Attack = 4.0f;

    // Starts at the parameters.md defaults.
    Compressor() noexcept;

    // Non-RT. No allocation either.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears the detectors and jumps every smoother to its target: two renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Values are clamped to the parameters.md ranges.
    void setThresholdDb(float dB) noexcept;
    void setRatio(float ratio) noexcept;
    void setKneeDb(float dB) noexcept;
    void setAttackMs(float ms) noexcept;
    void setReleaseMs(float ms) noexcept;
    void setBlendPct(float pct) noexcept;
    void setMakeupDb(float dB) noexcept;
    void setAutoMakeup(bool on) noexcept;
    void setMode(CompMode mode) noexcept;
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. in == out is allowed. Before prepare() it zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }
    // Gain reduction of the last processed sample, in dB (>= 0), for the meter.
    float getGainReductionDb() const noexcept { return grDb_; }

    // Static curve (Giannoulis et al. eq. 4): gain reduction in dB (>= 0) for a detector level xDb.
    static float staticGainReductionDb(float xDb, float thresholdDb, float ratio, float kneeDb) noexcept;

private:
    struct Detector
    {
        float attack = 0.0f, release = 0.0f, y = 0.0f;
        float next(float x) noexcept;
    };

    void updateCoefficients() noexcept;
    void updateAutoMakeup() noexcept;

    double sampleRate_ = 0.0;
    float ratio_ = 4.0f, kneeDb_ = 6.0f, attackMs_ = 10.0f, releaseMs_ = 150.0f, makeupDb_ = 0.0f;
    float thresholdTarget_ = -20.0f;
    bool autoMakeup_ = true, bypassed_ = false;
    CompMode modeFrom_ = CompMode::opto, modeTo_ = CompMode::opto, pendingMode_ = CompMode::opto;
    float grDb_ = 0.0f;

    Detector vca_, optoFast_, optoSlow_, dual1_, dual2_;
    OnePoleSmoother threshold_; // 20 ms, dB
    LinearRamp blend_;          // 20 ms, 0...1
    LinearRamp makeup_;         // 20 ms, linear gain of the manual makeup
    LinearRamp autoGain_;       // 10 ms (xf 10), linear gain of the auto makeup (1 when off)
    LinearRamp modeFade_;       // 10 ms (xf 10), 0 = modeFrom_, 1 = modeTo_
};

} // namespace osp::dsp
