#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Output block, `output.*` in parameters.md §3.6 (Global-Lock), signal-flow §6: stereo, latency 0 (L_out). Per
// sample, in this order:
// 1. NaN guard (realtime-rules RT-9): a non-finite sample becomes 0 and sets nanDetected(). Own decision: it runs on
//    the stage's input, before the limiter, whose gain state a NaN would poison; signal-flow §6 places it "after the
//    meters", and its intent (a NaN shows up as nanDetected, never as a meter spike) holds either way.
// 2. Level: output.levelDb, -60...+12 dB, a 20 ms linear ramp of the linear gain (parameters.md §1.4).
// 3. Mono sum: the global "mono sum" setting (settings file, signal-flow §2): L = R = (L + R) / 2, switched with a
//    10 ms linear crossfade.
// 4. Safety limiter, output.limiter (default on, xf 10): a -0.3 dBFS true-peak ceiling with no look-ahead, since
//    L_out = 0. Linked stereo gain, instant attack, kReleaseSeconds one-pole release that snaps to exactly 1, so it is
//    bit-exact below the ceiling. Sample peaks never exceed the ceiling (g <= ceiling / |x|). Peaks between samples
//    are estimated by 4x band-limited interpolation of the last kTpTaps samples (the true-peak method of ITU-R
//    BS.1770-4 Annex 2; here a Hann-windowed sinc, 12 taps per phase), which lags by kTpTaps / 2 samples. The release
//    holds the gain over that lag, so steady material has no overs; a transient can overshoot between samples during
//    its first kTpTaps / 2 samples only. Measured with 8x band-limited reconstruction in
//    tests/dsp/test_output_stage.cpp: sines 100 Hz-19 kHz up to +12 dBFS stay <= -0.15 dBTP; white noise 3.5 dB over
//    full scale reaches +2.4 dBTP and a naive square wave +0.7 dBTP between samples (sample peaks stay at the ceiling).
//    Closing that gap needs ~6 samples of look-ahead, which L_out = 0 rules out. A first version that fitted a parabola
//    through the last three samples let steady high-frequency sines through at up to +2.1 dBTP.
// 5. Meters: peak and RMS per channel of the last block, for the engine's telemetry atomics (threading.md §3.5).

#include "osp/dsp/util/Smoothers.h"

#include <array>

namespace osp::dsp {

class OutputStage {
public:
    static constexpr float kMinLevelDb = -60.0f, kMaxLevelDb = 12.0f;
    static constexpr float kCeilingDb = -0.3f;
    static constexpr double kReleaseSeconds = 0.050;
    static constexpr double kSwitchSeconds = 0.010; // limiter on/off and mono sum (xf 10)
    static constexpr int kTpTaps = 12; // true-peak interpolator length (per phase)

    OutputStage() noexcept;

    // Non-RT. No allocation.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Releases the limiter, jumps every ramp to its target and clears the meters: renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Clamped to -60...+12 dB; NaN gives -60.
    void setLevelDb(float dB) noexcept;
    void setLimiter(bool on) noexcept { limiterMix_.setTarget(on ? 1.0f : 0.0f); }
    void setMonoSum(bool on) noexcept { monoMix_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input (the NaN guard still runs: RT-9 has no exception).
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on both channels. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

    // Audio thread only (the engine copies them to its telemetry atomics).
    float peak(int channel) const noexcept { return peak_[channel == 0 ? 0 : 1]; }
    float rms(int channel) const noexcept { return rms_[channel == 0 ? 0 : 1]; }
    bool nanDetected() const noexcept { return nan_; }
    void clearNanDetected() noexcept { nan_ = false; }
    float gainReductionDb() const noexcept; // of the last sample, >= 0

private:
    double sampleRate_ = 0.0;
    bool bypassed_ = false, nan_ = false;
    LinearRamp level_{1.0f}, limiterMix_{1.0f}, monoMix_{0.0f};
    float ceiling_ = 1.0f, release_ = 0.0f, gain_ = 1.0f;
    std::array<std::array<float, kTpTaps>, 3> tp_{}; // 4x interpolator, phases 1/4, 2/4, 3/4
    std::array<std::array<float, kTpTaps>, 2> hist_{}; // the last kTpTaps samples per channel, before the limiter
    std::array<float, 2> peak_{}, rms_{};
};

} // namespace osp::dsp
