#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Feed-forward compressor, `comp.*` in parameters.md §2.3. Latency 0 (signal-flow §4: no look-ahead in v1).
//
// Algorithm: log-domain gain computer with a quadratic soft knee and a smooth branching peak detector applied to
// the gain reduction, as in D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design - A
// Tutorial and Analysis", J. Audio Eng. Soc. 60(6), 2012 (eqs. 4 and 17). Attack/release are the 0→63 % time
// constants of that detector (parameters.md §1.4).
//
// Modes (own design; the constants are not matched to any hardware, unverified):
// - vca:  one detector.
// - opto: program-dependent two-stage release. A fast detector (attack, release) runs in parallel with a slow one
//         that tracks kOptoSlowDepth of the gain reduction with kOptoSlowAttack x attack and kOptoSlowRelease x
//         release. GR = max(fast, slow): short peaks release fast; sustained compression charges the slow stage,
//         so the release drops quickly to the slow level and then tails off.
// - dual: two stages in series, each with ratio sqrt(R) and the same threshold, so the hard-knee static curve is
//         exactly R. The second stage sees the first stage's output and has 4x the attack. With a soft knee the
//         two knees compound, so inside the knee the curve differs slightly from a single-stage R (outside it is
//         exactly R).
//
// Low-note distortion (#89, a decision, not a defect): the detector is a peak detector on the GR, so with a short
// release it sags between the peaks of a low note and re-attacks on each one, modulating the gain at 2 f0 and adding
// odd harmonics. Release sets it (at 0.1 ms attack THD drops 7, 9, 10 dB per step 20 -> 50 -> 150 -> 500 ms); a slower
// attack helps less. Measured at 82.8 Hz, -12 dBFS, defaults otherwise (about 6 dB of GR), modes within 1.3 dB: -30.5
// dB (3 %) at 0.1 / 20 ms, -50.1 dB (0.3 %) at the 10 / 150 ms defaults. Kept: a hold or RMS floor would stretch the
// release past its 63 % time constant (parameters.md §1.4) and read a sine below its peak, and a fast release that
// growls on low notes is what that corner of the range is for. test_compressor.cpp gates the grid at measured + 3 dB.
//
// Auto makeup = the static gain reduction at the DI nominal peak level kAutoMakeupRefDb (tone-targets-a §3.4), so
// a DI at nominal level leaves at the same peak. It adds to the manual makeup. Makeup applies to the wet path.
// Its dynamics (#160, own design): the makeup must not lead the gain reduction, or a lower threshold (or a higher
// ratio, a harder knee) is a burst: the old 10 ms ramp to the new target gave +13.4 dB on a -30 -> -50 dB step,
// while the GR still waited on the 20 ms threshold smoother and the attack. So the auto makeup in dB is recomputed
// every sample from the *smoothed* threshold and the current ratio/knee, and follows that target through a branching
// one-pole like the detector's. It rises with 2 x the attack time constant (1.5 x more in dual, whose second stage
// attacks 4 x slower; at least 5 ms): on a real signal the peak detector only charges near the peaks, so the GR rises
// more slowly than its time constant. It falls with 0.25 x the release (at least 5 ms): on a real signal the GR also
// releases toward the troughs, so it falls faster than its time constant. Both err toward a short dip, never a burst.
// Measured (220 Hz sine at -12 dBFS, defaults, block 64; test_compressor.cpp "[makeup]"): threshold -30 -> -50 dB
// overshoot +0.05 dB (was +13.4), dip -0.6 dB (vca, opto) / -1.4 dB (dual); -30 -> -10 dB dip -5.3 dB (was -12.0);
// ratio 4 -> 20 +0.1 dB (was +2.0). Worst overshoot over attack 0.1...100 ms, release 20...1000 ms, 82 Hz...1 kHz,
// all modes and threshold/ratio/knee steps: +0.7 dB (dual, 82 Hz, 0.1 ms attack, 20 ms release, threshold up).
// Steady state is unchanged. The on/off switch stays the xf 10 linear-gain fade.
//
// Tone: first-order tilt at the comp output, H(s) = (G s + w0) / (s + G w0) with w0 = 2 pi kTiltPivotHz and
// G = 10^(t / 20), t = (tone - 5) * 1.2 dB: -t dB at DC, +t dB at HF, exactly 0 dB at the pivot. Bilinear
// transform prewarped at the pivot (R. Bristow-Johnson, "Audio EQ Cookbook", W3C Audio WG note, 2016). Direct
// form I, so tone 5 (G = 1) is an exact identity. While the knob moves the coefficients are recomputed every
// sample (parameters.md §1.4): a 16-sample hold measured -55 dBc on a 0 <-> 10 step, above the REQ-TEST-003 gate.

#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp {

enum class CompMode { opto, vca, dual };

class Compressor {
public:
    static constexpr float kAutoMakeupRefDb = -12.0f;
    // Auto-makeup follower (#160): rise tau = kAutoMakeupRiseScale x attack (x kAutoMakeupDualScale in dual), at least
    // kAutoMakeupMinRiseMs; fall tau = kAutoMakeupFallScale x release, at least kAutoMakeupMinFallMs.
    static constexpr float kAutoMakeupRiseScale = 2.0f;
    static constexpr float kAutoMakeupDualScale = 1.5f;
    static constexpr float kAutoMakeupMinRiseMs = 5.0f;
    static constexpr float kAutoMakeupFallScale = 0.25f;
    static constexpr float kAutoMakeupMinFallMs = 5.0f;
    static constexpr float kOptoSlowDepth = 0.5f;
    static constexpr float kOptoSlowAttack = 20.0f;
    static constexpr float kOptoSlowRelease = 8.0f;
    static constexpr float kDualStage2Attack = 4.0f;
    static constexpr double kTiltPivotHz = 1000.0;

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
    // comp.tone, 0...10 knob, 5 = flat. One-pole 20 ms on the knob.
    void setTone(float knob) noexcept;
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. in == out is allowed. Before prepare() it zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }
    // Gain reduction of the last processed sample, in dB (>= 0), for the meter. Audio thread only: the engine
    // publishes it to the UI through its telemetry atomics (threading.md), never by reading it from another thread.
    float getGainReductionDb() const noexcept { return grDb_; }

    // Static curve (Giannoulis et al. eq. 4): gain reduction in dB (>= 0) for a detector level xDb.
    static float staticGainReductionDb(float xDb, float thresholdDb, float ratio, float kneeDb) noexcept;

private:
    struct Detector {
        float attack = 0.0f, release = 0.0f, y = 0.0f;
        float next(float x) noexcept;
    };

    void updateCoefficients() noexcept;
    float autoMakeupTargetDb(float thresholdDb) const noexcept;
    void updateTilt(float knob) noexcept;
    void processTilt(float* out, int numSamples) noexcept;

    double sampleRate_ = 0.0;
    float ratio_ = 4.0f, kneeDb_ = 6.0f, attackMs_ = 10.0f, releaseMs_ = 150.0f, makeupDb_ = 0.0f;
    float thresholdTarget_ = -20.0f;
    bool autoMakeup_ = true, bypassed_ = false;
    CompMode modeFrom_ = CompMode::opto, modeTo_ = CompMode::opto, pendingMode_ = CompMode::opto;
    float grDb_ = 0.0f;

    Detector vca_, optoFast_, optoSlow_, dual1_, dual2_;
    OnePoleSmoother threshold_; // 20 ms, dB
    LinearRamp blend_; // 20 ms, 0...1
    LinearRamp makeup_; // 20 ms, linear gain of the manual makeup
    LinearRamp autoOn_; // 10 ms (xf 10), 0 = auto makeup off, 1 = on
    float autoDb_ = 0.0f; // applied auto makeup in dB: the lagged follower of autoMakeupTargetDb()
    float autoGain_ = 1.0f; // dbToGain(autoDb_)
    float autoRise_ = 0.0f, autoRiseDual_ = 0.0f, autoFall_ = 0.0f; // follower coefficients
    LinearRamp modeFade_; // 10 ms (xf 10), 0 = modeFrom_, 1 = modeTo_
    OnePoleSmoother tone_; // 20 ms, knob
    double tiltK_ = 0.0; // 1 / tan(pi f0 / fs)
    float b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f, x1_ = 0.0f, y1_ = 0.0f;
    float tiltKnob_ = 5.0f; // knob the coefficients were computed for
};

} // namespace osp::dsp
