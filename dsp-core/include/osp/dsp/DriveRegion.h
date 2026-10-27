#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// OS-1, the shared drive domain (ADR-0005 D2, signal-flow §3): one Oversampler and the two drive slots in series,
// `drive1` then `drive2`. The factor is that of the hungriest voicing either slot can hold, so a type change or an
// enable/bypass never changes the factor or the latency (REQ-303):
//   Live: 4x at 44.1/48 kHz, 2x at 88.2/96 kHz. HQ: 8x at any rate (ADR-0005 D2; it wins over signal-flow's
//   "2x the Live factor" wording). Live runs the IIR filters, HQ the FIR ones (ADR-0005 D1/D3).
// Latency (ADR-0005 D4): D = the oversampler's measured 1 kHz delay + (2 slots x DriveStage::kDelaySamples + pad) /
// factor, reported as round(D). Each slot is an exact 1-sample delay at the oversampled rate, so D needs no extra
// measurement. With the FIR kind the oversampler delay is an integer (101/116/124 at 2x/4x/8x, Amendment A3), and the pad (6 samples
// at 8x, Amendment A2) rounds the slots up to a whole base sample: HQ is then an exact pure delay of 125 samples.
// A quality switch re-prepares (ADR-0005 D6) and fades the new chain in over kSwitchFadeMs, so its output rises from
// silence without a step (silent for the new latency, then a linear ramp); fading the old chain out before the
// re-prepare is the caller's job.

#include "osp/dsp/Drive.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "osp/dsp/util/DelayLine.h"

#include <array>

namespace osp::dsp {

enum class OversamplingQuality { live, hq };

class DriveRegion {
public:
    static constexpr int kNumSlots = 2;
    static constexpr double kSwitchFadeMs = 10.0;

    // The OS-1 factor for a host rate and quality mode (ADR-0005 D2).
    static int factorFor(double sampleRate, OversamplingQuality quality) noexcept;

    // Both slots start disabled (parameters.md §1.6).
    DriveRegion();

    // Non-RT: allocates. A quality or rate change goes through here only (ADR-0005 D6, REQ-OS-006).
    void prepare(double sampleRate, int maxBlockSize, OversamplingQuality quality = OversamplingQuality::live);
    // RT-safe. Two renders after reset() are bit-identical.
    void reset() noexcept;

    // Slot 0 = drive1, slot 1 = drive2. Set parameters on them once per block (RT-safe setters).
    DriveStage& slot(int index) noexcept { return slots_[index == 0 ? 0 : 1]; }

    // Hard bypass for tests (signal-flow §5): output = input delayed by getLatencySamples().
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. in == out is allowed. Before prepare() it zero-fills.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getFactor() const noexcept { return oversampler_.getFactor(); }
    OversamplingFilter getFilter() const noexcept { return oversampler_.getFilter(); }
    int getPadSamples() const noexcept { return padSamples_; } // at the oversampled rate
    int getLatencySamples() const noexcept { return latency_; }
    double getMeasuredDelaySamples() const noexcept { return delay_; }
    bool isPrepared() const noexcept { return oversampler_.isPrepared(); }

private:
    Oversampler oversampler_;
    std::array<DriveStage, kNumSlots> slots_;
    DelayLine bypassDelay_;
    DelayLine pad_; // at the oversampled rate, after the slots
    double delay_ = 0.0;
    int latency_ = 0, padSamples_ = 0;
    int fadeLength_ = 0, fadePos_ = 0; // fade-in after a quality switch, restarted by reset()
    OversamplingQuality quality_ = OversamplingQuality::live;
    bool bypassed_ = false;
};

} // namespace osp::dsp
