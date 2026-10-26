#pragma once

// Noise gate (parameters.md §2.2, issue #65). Pure C++20: no JUCE, no OS headers. Zero latency.
//
// Algorithm: level detector (peak: instant attack with a 10 ms exponential decay; RMS: one-pole mean square,
// τ = 10 ms) → open/hold/closed state machine with hysteresis (opens at the threshold, stays open while the level is
// above threshold − hysteresis, then counts the hold time down) → one-pole gain smoothing towards 1 (open) or the
// range floor (closed), with the attack τ when rising and the release τ when falling.
// Sources: J. D. Reiss & A. McPherson, "Audio Effects: Theory, Implementation and Application", CRC Press 2014,
// ch. 6 (noise gate: threshold, hysteresis, hold, attack/release); D. Giannoulis, M. Massberg, J. D. Reiss,
// "Digital Dynamic Range Compressor Design — A Tutorial and Analysis", JAES 60(6), 2012 (peak/RMS detectors and
// one-pole attack/release smoothing). The 10 ms detector time is an engineering choice (*unverified* by listening).
//
// The gain can be applied to the input (process) or computed from the input as a key and applied elsewhere, for
// gate.postAmp, where the engine wires the tap after the amp (computeGain + applyGain).

#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/Smoothers.h"

#include <vector>

namespace osp::dsp {

class Gate {
public:
    enum class Detector { Peak, Rms };

    // Plain values as in parameters.md §2.2 (defaults included). setParameters() clamps to the specified ranges.
    struct Params {
        float thresholdDb = -55.0f; // −90…−20, smoothed 20 ms
        float hysteresisDb = 4.0f;  // 0…10
        float attackMs = 0.5f;      // 0.1…10
        float holdMs = 20.0f;       // 0…200
        float releaseMs = 80.0f;    // 10…500
        float rangeDb = -90.0f;     // −90…0, smoothed 20 ms
        Detector detector = Detector::Peak;
    };

    // Non-RT. Allocates one scratch buffer of maxBlockSize samples.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Gate closed, gain at the range floor, smoothers at their targets.
    void reset() noexcept;
    // RT-safe. Called once per block with values read from the parameter atomics.
    void setParameters(const Params& params) noexcept;
    // RT-safe. gate.enabled: a 10 ms constant-amplitude crossfade (signal-flow.md §5). Disabled and settled is an
    // exact copy of the input (hard-bypass null).
    void setEnabled(bool enabled) noexcept;

    // RT-safe. Gates `in` with itself as the key. in == out is allowed; any n >= 0. Before prepare() it zero-fills.
    void process(const float* in, float* out, int numSamples) noexcept;
    // RT-safe. Post-amp tap: writes the per-sample gain derived from `key` (ignores enable; the engine fades the tap).
    void computeGain(const float* key, float* gain, int numSamples) noexcept;
    static void applyGain(const float* gain, float* io, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isOpen() const noexcept { return open_; }
    float currentGain() const noexcept { return target_ + deviation_; }

private:
    double sampleRate_ = 0.0;
    std::vector<float> scratch_;
    Params params_;
    OnePoleSmoother thresholdLin_; // linear amplitude
    OnePoleSmoother rangeLin_;     // linear gain
    Crossfader enable_;
    float closeRatio_ = 1.0f; // 10^(−hysteresis/20)
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
    float detectorCoeff_ = 0.0f;
    int holdSamples_ = 0;
    float envelope_ = 0.0f; // peak amplitude, or mean square for RMS
    // Gain = target_ + deviation_. The deviation decays geometrically, so the gain never stalls short of its target
    // in float precision (with τ = 500 ms at 96 kHz the plain form would stall up to 1.5e-3 away).
    float target_ = 0.0f;
    float deviation_ = 0.0f;
    int holdLeft_ = 0;
    bool open_ = false;
};

} // namespace osp::dsp
