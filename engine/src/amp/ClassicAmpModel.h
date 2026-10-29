#pragma once

// Classic-DSP amp model (ADR-0011 D1-D3, slice P5-8 / #174). No JUCE, no NAM.
//
// Signal path, mono. At the host rate:  input sanitise -> x kGridAt0Dbfs (level calibration, Voicings.h) -> tight
// HPF -> bright shelf.  Inside OS-2 (ADR-0005 D2: Live 4x at 44.1/48 kHz, 2x at 88.2/96 kHz, HQ 8x; IIR Live, FIR
// HQ, the same Oversampler and factor table as DriveRegion):  preamp stages -> tone stack -> remaining stages ->
// power amp.  At the host rate again:  output trim x levelDb (20 ms linear ramp of the linear gain).
// - The tone-stack insertion-loss makeup and the power-amp input gain are folded into the drive of the stage that
//   follows (no extra multiply pass).
// - bright: a 1st-order high shelf at brightHz, brightDb·(1 − gain/10) dB (a treble-bleed cap on the volume pot,
//   research §5); tight: a 1st-order HPF at tightHz ahead of S1, plus a cathode-corner shift on the shelved stages.
//   Both filters always run; the switches move their mix amounts over a 10 ms linear ramp (parameters.md "xf 10"),
//   so a toggle never steps the signal or the ADAA state downstream (research Q12). The bright amount follows the
//   gain knob through the same ramp.
// - Delay (ADR-0011 D2, ADR-0005 D4; orchestrator decision on PR #191): the PROCESSING delay only, as DriveRegion
//   and NAM report it: the oversampler's 1 kHz round trip measured in prepare() plus the ADAA terms (1 stage-rate
//   sample per preamp stage, 0.5 for the power amp), in host-rate samples. getLatencySamples() = ceil(D). The
//   modelled filters' 1 kHz group delay (coupling, Miller, cathode, tone stack, NFB loop, transformer) is the sound,
//   not latency; getFilterGroupDelaySamples() reports it at the parameters.md defaults as a diagnostic (from
//   PreampStage::groupDelaySamples, the designed ToneStack coefficients and PowerAmp::response, ADAA terms removed).

#include "osp/dsp/amp/Voicings.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/engine/AmpModel.h"
#include "osp/engine/AmpSlot.h" // AmpParams

#include <array>

namespace osp::engine {

class ClassicAmpModel final : public IAmpModel {
public:
    static constexpr double kSwitchSeconds = 0.010; // bright/tight crossfade (parameters.md §2.5)
    static constexpr double kLevelSeconds = 0.020; // levelDb ramp

    explicit ClassicAmpModel(ClassicAmpId id);

    void prepare(double hostSampleRate, int maxBlockSize, dsp::OversamplingQuality quality) override;
    void process(const float* in, float* out, int numSamples) noexcept OSP_RT_NONBLOCKING override;
    void reset() noexcept OSP_RT_NONBLOCKING override;
    double getMeasuredDelaySamples() const noexcept override { return delay_; }
    // Diagnostic: the modelled filters' small-signal 1 kHz group delay at the defaults, host-rate samples.
    double getFilterGroupDelaySamples() const noexcept { return filterDelay_; }
    int getLatencySamples() const noexcept override { return latency_; }
    const AmpModelInfo& info() const noexcept override { return info_; }

    // RT-safe, once per block (non-virtual, ADR-0011 D2). Values are clamped by the stages; NaN maps to a range end.
    void setParameters(const AmpParams& params) noexcept OSP_RT_NONBLOCKING;

    ClassicAmpId id() const noexcept { return id_; }
    const dsp::amp::AmpVoicing& voicing() const noexcept { return voicing_; }
    int getFactor() const noexcept { return os_.getFactor(); }
    bool isPrepared() const noexcept { return os_.isPrepared(); }

private:
    void applyParameters() noexcept;
    void runOversampled(float* os, int numSamples) noexcept;
    double linearGroupDelay() const noexcept; // at the current targets, host-rate samples

    ClassicAmpId id_;
    const dsp::amp::AmpVoicing& voicing_;
    AmpModelInfo info_;
    dsp::Oversampler os_;
    std::array<dsp::amp::PreampStage, dsp::amp::kMaxPreampStages> stages_;
    dsp::amp::ToneStack stack_;
    dsp::amp::PowerAmp power_;
    AmpParams params_{};

    double fs_ = 0.0;
    double brightG_ = 0.0, tightG_ = 0.0; // TPT one-pole G = g/(1 + g) at brightHz / tightHz
    double brightState_ = 0.0, tightState_ = 0.0;
    dsp::LinearRamp brightMix_{0.0f}, tightMix_{0.0f}, level_{1.0f};
    double delay_ = 0.0, filterDelay_ = 0.0;
    int latency_ = 0;
};

} // namespace osp::engine
