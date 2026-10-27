#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Audio-thread side of the chromatic tuner, REQ-419 and signal-flow §6: the tap reads the mono input after the input
// trim and before the gate, adds no latency, and hands a decimated copy to PitchDetector on a worker thread.
// - Anti-alias: 4th-order Butterworth low pass at kLowPassHz (two TPT SVF sections, Q 0.5412 and 1.3066), at the
//   host rate. The tuner only needs the fundamental (<= 1 kHz) and a few harmonics; the low pass also keeps the YIN
//   difference function smooth.
// - Decimation by D = round(fs / kTargetRate), at least 1: 22.05 kHz at 44.1/88.2 kHz, 24 kHz at 48/96 kHz.
// - Hand-off: a single-producer ring of kRingSize std::atomic<float> plus a release-stored write count. The worker
//   copies the latest window with readLatest(); a copy the producer overran meanwhile is reported as failed and
//   simply retried on the next poll, so the audio thread never waits (RT-1/RT-4).
// - Output mute (tuner.muteOutput, parameters.md §4): applyMute() ramps the output to silence over kMuteSeconds and
//   back; unmuted and idle it does not touch the samples, so it is bit-exact.

#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace osp::dsp {

class TunerTap {
public:
    static constexpr double kTargetRate = 24000.0;
    static constexpr double kLowPassHz = 5000.0;
    static constexpr int kRingSize = 1 << 14; // 0.68 s at 24 kHz: over 10x PitchDetector's window
    static constexpr double kMuteSeconds = 0.010;

    // Non-RT. No allocation (the ring is a member).
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears the filters, the decimation phase and the ring, and jumps the mute to its target.
    void reset() noexcept;

    // RT-safe, audio thread. Feeds n input samples into the tap; non-finite samples count as 0. Before prepare()
    // it does nothing.
    void push(const float* in, int numSamples) noexcept;
    // RT-safe, audio thread. tuner.muteOutput while the tuner is open: a kMuteSeconds linear ramp to 0 and back.
    void setMuted(bool muted) noexcept { mute_.setTarget(muted ? 0.0f : 1.0f); }
    void applyMute(float* io, int numSamples) noexcept;

    // Worker thread. Copies the latest n decimated samples (n <= kRingSize / 2) into dst. False when fewer have
    // been pushed since reset(), or when the producer overwrote part of the copy (try again on the next poll).
    bool readLatest(float* dst, int n) const noexcept;

    double decimatedRate() const noexcept { return sampleRate_ / factor_; }
    int decimationFactor() const noexcept { return factor_; }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

private:
    double sampleRate_ = 0.0;
    int factor_ = 1, phase_ = 0;
    SvfCoeffs lp1_, lp2_;
    SvfState s1_, s2_;
    LinearRamp mute_{1.0f};
    std::uint32_t produced_ = 0; // audio thread's copy of written_
    std::atomic<std::uint32_t> written_{0};
    std::array<std::atomic<float>, kRingSize> ring_{};
};

} // namespace osp::dsp
