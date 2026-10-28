#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3).
//
// The v1 signal graph (ADR-0009, docs/spec/signal-flow.md §1):
//   in (mono) → input → [tuner] → gate → comp → OS-1 {drive1 → drive2} → mod (pre, mono)
//   → strip[amp.channel] {amp slot → cab (mono → stereo) → EQ} → mod (post) → delay → reverb → doubler → output.
// Every block is held by value; prepare() is the only call that allocates. Parameters are read from a ParamStore
// once per block (threading.md §3) and fanned out to the block setters (ADR-0009 D8).

#include "osp/engine/AmpSlot.h"
#include "osp/engine/Params.h"

#include "osp/dsp/Cab.h"
#include "osp/dsp/Compressor.h"
#include "osp/dsp/Delay.h"
#include "osp/dsp/Doubler.h"
#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/Eq.h"
#include "osp/dsp/Gate.h"
#include "osp/dsp/InputStage.h"
#include "osp/dsp/Modulation.h"
#include "osp/dsp/OutputStage.h"
#include "osp/dsp/Reverb.h"
#include "osp/dsp/tuner/TunerTap.h"
#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/DelayLine.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace osp::engine {

class Graph {
public:
    static constexpr int kNumChannels = 3; // amp channels, each a strip {amp, cab, EQ} (ADR-0009 D1)
    static constexpr double kSwapSeconds = 0.025; // ADR-0004 §5 owns this value

    struct Options {
        double ampStubDelaySamples = 0.0; // AmpStub test delay (ADR-0009 D5)
    };

    // Control thread. `params` must outlive the graph.
    explicit Graph(const ParamStore& params, Options options = {});
    ~Graph();
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    // Non-RT: allocates. A rate, block-size or quality change goes through here only.
    void
    prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality = dsp::OversamplingQuality::live);
    // RT-safe. Two renders after reset() are bit-identical.
    void reset() noexcept;
    // RT-safe. Mono in, stereo out, 0 <= numSamples <= maxBlockSize. `in` may alias outL (not outR).
    void process(const float* in, float* outL, float* outR, int numSamples) noexcept;

    // RT-safe global settings (not presets, parameters.md §4).
    void setTempoBpm(double bpm) noexcept { tempoBpm_ = bpm; }
    void setMonoSum(bool on) noexcept { output_.setMonoSum(on); }
    void setTunerMuted(bool muted) noexcept { tuner_.setMuted(muted); }

    // RT-safe. Installs a convolver prepared at this rate in cab.ch<channel+1>.ir<slot+1> (ADR-0004 §4); the graph
    // never deletes it. Drain popRetired() after each call and each process().
    void setCabIr(int channel, int slot, dsp::PartitionedConvolver* ir) noexcept;
    dsp::PartitionedConvolver* popRetired() noexcept;

    // Hard bypass of every block (tests, signal-flow §5): the output is the input delayed by exactly
    // getLatencySamples() (REQ-TEST-004). RT-safe.
    void setBypassedAll(bool bypassed) noexcept;

    // L_total = round(D_os1 + D_slot + D_cab), rounded once (ADR-0009 D4). Fixed after prepare().
    int getLatencySamples() const noexcept { return latency_; }
    // The fractional sum it is rounded from, and the clean DI delay K_clean (ADR-0009 D4, D7).
    double getMeasuredDelaySamples() const noexcept { return measuredDelay_; }
    int getCleanDelaySamples() const noexcept { return clean_.getDelay(); }
    // The gate.postAmp gain delay K_gate = round(D_os1 + D_slot) (ADR-0009 D4, D6).
    int getGateTapDelaySamples() const noexcept { return gatePost_.getDelay(); }
    int activeChannel() const noexcept { return stripTo_; }
    const dsp::OutputStage& output() const noexcept { return output_; }
    const dsp::TunerTap& tuner() const noexcept { return tuner_; }

private:
    struct Ids;
    struct Strip {
        std::unique_ptr<AmpChannel> amp;
        dsp::Cab cab;
        dsp::Eq eq;
        dsp::Crossfader eqOnL, eqOnR; // eq.enabled (the Eq block has no enable of its own, ADR-0009 D3)
        bool eqIdle = false;
    };

    float value(int index) const noexcept
    {
        return sanitize(table_[static_cast<std::size_t>(index)], store_.get(index));
    }
    bool on(int index) const noexcept { return value(index) > 0.5f; }
    int choice(int index) const noexcept { return static_cast<int>(value(index)); }

    void applyParameters() noexcept; // Fanout.cpp
    void applyStrip(int strip) noexcept; // Fanout.cpp
    void applyChannelSwitch() noexcept;
    void applyModPosition(int numSamples) noexcept;
    void processGate(float* io, int n) noexcept;
    void processStrip(Strip& s, const float* in, float* outL, float* outR, int n) noexcept;
    void processChunk(float* io, float* outL, float* outR, int n) noexcept;

    const ParamStore& store_;
    std::span<const ParamInfo> table_;
    std::unique_ptr<Ids> ids_;

    dsp::InputStage input_;
    dsp::Crossfader inputOn_; // input.enabled
    dsp::TunerTap tuner_;
    dsp::Gate gate_;
    // gate.enabled and gate.postAmp (ADR-0009 D6): 10 ms ramps e and x; the post-amp factor h^x is delayed by
    // K_gate and applied after the amp slot.
    dsp::LinearRamp gateOn_, gateTap_;
    dsp::DelayLine gatePost_;
    dsp::Compressor comp_;
    dsp::Crossfader compOn_; // comp.enabled
    bool inputIdle_ = false, compIdle_ = false;
    dsp::DriveRegion drives_;
    dsp::Modulation mod_;
    std::array<Strip, kNumChannels> strips_;
    dsp::Delay delay_;
    dsp::Reverb reverb_;
    dsp::Doubler doubler_;
    dsp::OutputStage output_;

    // Clean DI (amp.cleanBlendPct, ADR-0009 D7): the amp input delayed by K_clean, mixed after the strips.
    dsp::DelayLine clean_;
    dsp::LinearRamp cleanBlend_; // 0…1, 20 ms
    bool bypassedAll_ = false;
    int slotPad_ = 0; // the amp slot's hard-bypass pad, R(amp out) − R(amp in)
    double measuredDelay_ = 0.0;

    // mod.position: the block's wet fades out, then it moves (ADR-0009 D1).
    bool modPre_ = false;
    int modMoveLeft_ = 0, modMoveSamples_ = 0;

    // amp.channel switch: strip stripFrom_ fades to stripTo_ over kSwapSeconds; a change during a fade waits.
    int stripFrom_ = 0, stripTo_ = 0;
    dsp::LinearRamp stripFade_{1.0f};

    double sampleRate_ = 0.0, tempoBpm_ = 120.0;
    int maxBlock_ = 0, latency_ = 0;
    std::vector<float> mono_, dry_, dryR_, stripL_, stripR_, cleanBuf_, postGain_;
};

} // namespace osp::engine
