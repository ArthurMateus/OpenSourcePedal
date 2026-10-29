#pragma once

// Private to osp_engine: the Phase 5 amp slot (ADR-0011 D1/D2, slice P5-9 / #175, classic-first scope).
//
// One ModelAmpChannel per channel strip. It owns the three built-in classic models (osp.amp.clean_a / crunch_a /
// lead_a), all built and prepared in prepare(), so a model change is a 25 ms linear crossfade (Graph::kSwapSeconds,
// ADR-0004 §5) between two prepared models: the incoming one is reset (cold start, like a strip on a channel switch)
// and both run during the fade. No worker load is needed for built-ins; NAM models arrive with #169/#176.
//
// Signal path, mono, in place:
//   input sanitise -> amp.ch<N>.lowCutHz (ADR-0011 D4) -> active model (and the incoming one during a fade)
//   -> per-model pad to D_slot -> linear crossfade.
// - D_slot = ceil(max over the classic models of their processing delay at (fs, quality)) (ADR-0011 D2 and A2: the
//   OS-2 round trip plus the ADAA terms, not the modelled filters' phase). getMeasuredDelaySamples() returns it, so
//   every channel and every model reports the same integer slot delay (ADR-0002 D4, ADR-0009 D5).
// - Pad of model k: p_k = D_slot - D_k >= 0, an integer DelayLine of n_k samples plus a first-order Thiran allpass
//   of d_k = p_k - n_k (d_k in [0.5, 1.5) whenever p_k >= 0.5, else d_k = p_k). The ADR names a FractionalDelay;
//   a static pad needs no modulation, and the allpass keeps the magnitude exactly flat where the Catmull-Rom/linear
//   taps of dsp::FractionalDelay droop (linear interpolation at d = 0.5 is cos(w/2): -4.6 dB at 15 kHz / 48 kHz).
//   The allpass delay is exact at DC and within 0.001 sample at 1 kHz for every rate here.
//   Source: J.-P. Thiran, "Recursive digital filters with maximally flat group delay", IEEE Trans. Circuit Theory
//   18(6), 1971; T. I. Laakso, V. Valimaki, M. Karjalainen, U. K. Laine, "Splitting the unit delay", IEEE Signal
//   Processing Magazine 13(1), 1996, section on allpass interpolation (a = (1 - d)/(1 + d)).
// - lowCutHz: 20...300 Hz, 20 = off. A 2nd-order Butterworth TPT SVF high-pass (dsp/util/Svf.h, as the drive low
//   cut), its frequency on a 20 ms one-pole (parameters.md §1.4, filter frequency), coefficients redesigned per
//   sample while it moves. Its output is crossfaded with the dry input over 20 ms on the off <-> on edge, so settled
//   off (20 Hz) is the exact input and the filter is not run.
// - levelDb is forwarded with the other knobs: ClassicAmpModel applies it as its 20 ms linear output ramp, the same
//   law AmpStub uses, so the channel does not apply it a second time. NAM-channel controls are a follow-up (#169).
// - Hard bypass (tests): an exact integer delay of the graph's pad; the models are not run.

#include "../amp/ClassicAmpModel.h"

#include "osp/dsp/util/DelayLine.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"
#include "osp/engine/AmpSlot.h"

#include <array>
#include <memory>
#include <vector>

namespace osp::engine {

class ModelAmpChannel final : public AmpChannel {
public:
    static constexpr int kNumModels = 3; // ClassicAmpId order
    static constexpr double kSwapSeconds = 0.025; // ADR-0004 §5 (= Graph::kSwapSeconds)
    static constexpr double kLowCutSeconds = 0.020;
    static constexpr float kLowCutOffHz = 20.0f, kLowCutMaxHz = 300.0f;

    // Control thread; allocates the three models (unprepared).
    explicit ModelAmpChannel(ClassicAmpId initial);

    void prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality) override;
    void reset() noexcept OSP_RT_NONBLOCKING override;
    void setParameters(const AmpParams& params) noexcept OSP_RT_NONBLOCKING override;
    void setBypassed(bool bypassed, int padSamples) noexcept OSP_RT_NONBLOCKING override;
    void process(float* io, int numSamples) noexcept OSP_RT_NONBLOCKING override;
    double getMeasuredDelaySamples() const noexcept OSP_RT_NONBLOCKING override { return slotDelay_; }

    // RT-safe. Crossfades to model `id` over kSwapSeconds from the next process(). A change during a fade starts
    // when that fade ends (the latest request wins), as the graph does for amp.channel.
    void setModel(ClassicAmpId id) noexcept OSP_RT_NONBLOCKING;
    ClassicAmpId activeModel() const noexcept OSP_RT_NONBLOCKING { return static_cast<ClassicAmpId>(to_); }
    bool isSwapping() const noexcept OSP_RT_NONBLOCKING { return fade_.isRamping(); }

    // Diagnostics for tests: a model, and its pad p_k = D_slot - D_k.
    const ClassicAmpModel& model(ClassicAmpId id) const noexcept { return *models_[index(id)]; }
    double padSamples(ClassicAmpId id) const noexcept { return pads_[index(id)].total; }

private:
    // Integer delay + first-order Thiran allpass (see the header comment).
    struct Pad {
        dsp::DelayLine line;
        double total = 0.0;
        float a = 0.0f, x1 = 0.0f, y1 = 0.0f;
        bool allpass = false;
        void prepare(double pad, int maxDelay);
        void reset() noexcept { line.reset(), x1 = y1 = 0.0f; }
        void process(float* io, int n) noexcept;
    };

    static std::size_t index(ClassicAmpId id) noexcept { return static_cast<std::size_t>(id); }
    void startPendingSwap() noexcept;
    void processLowCut(float* io, int n) noexcept;

    std::array<std::unique_ptr<ClassicAmpModel>, kNumModels> models_;
    std::array<Pad, kNumModels> pads_;
    AmpParams params_{};
    int from_ = 0, to_ = 0, wanted_ = 0;
    dsp::LinearRamp fade_{1.0f};

    double fs_ = 0.0, slotDelay_ = 0.0;
    int maxBlock_ = 0;
    std::vector<float> in_, a_, b_;

    dsp::OnePoleSmoother lowCutHz_;
    dsp::LinearRamp lowCutOn_{0.0f};
    dsp::SvfCoeffs lowCutCoeffs_{};
    dsp::SvfState lowCutState_{};
    float lowCutDesignedHz_ = 0.0f;

    dsp::DelayLine bypass_;
    bool bypassed_ = false;
};

} // namespace osp::engine
