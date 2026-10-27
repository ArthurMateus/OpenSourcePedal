#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3).
//
// The amp slot of one amp channel (ADR-0009 D5). Phase 5 puts the classic-DSP amp and the NAM channel behind this
// interface; until then every channel is an AmpStub. All channels and model kinds report the same delay
// (ADR-0002 D4: one L_slot). A virtual call to an already-built object is RT-allowed (realtime-rules §1).

#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/util/FractionalDelay.h"
#include "osp/dsp/util/Smoothers.h"

namespace osp::engine {

// Plain values of amp.ch<N>.* (parameters.md §2.5).
struct AmpParams {
    float gain = 5.0f, bass = 5.0f, mid = 5.0f, treble = 5.0f, presence = 5.0f, depth = 5.0f, master = 5.0f;
    float levelDb = 0.0f;
    bool bright = false, tight = false;
};

class AmpChannel {
public:
    virtual ~AmpChannel() = default;
    // Non-RT. May allocate.
    virtual void prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality) = 0;
    virtual void reset() noexcept = 0;
    // RT-safe, once per block.
    virtual void setParameters(const AmpParams& params) noexcept = 0;
    // Hard bypass for tests: output = input delayed by `padSamples`, the integer the graph assigns (ADR-0009 D4).
    virtual void setBypassed(bool bypassed, int padSamples) noexcept = 0;
    // RT-safe, mono, in place, any 0 <= n <= maxBlockSize.
    virtual void process(float* io, int numSamples) noexcept = 0;
    // Measured 1 kHz delay in base-rate samples, fixed after prepare().
    virtual double getMeasuredDelaySamples() const noexcept = 0;
};

// v1 stand-in: identity plus the tone-neutral channel level (amp.ch<N>.levelDb, 20 ms linear ramp). Every other
// amp.ch<N>.* value is ignored. `testDelaySamples` > 0 adds a cubic fractional delay, so the graph's alignment can
// be tested with a non-integer slot delay before Phase 5.
class AmpStub final : public AmpChannel {
public:
    explicit AmpStub(double testDelaySamples = 0.0) noexcept
        : testDelay_(testDelaySamples > 0.0 ? testDelaySamples : 0.0)
    {}

    void prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality) override;
    void reset() noexcept override;
    void setParameters(const AmpParams& params) noexcept override;
    void setBypassed(bool bypassed, int padSamples) noexcept override;
    void process(float* io, int numSamples) noexcept override;
    double getMeasuredDelaySamples() const noexcept override { return testDelay_; }

private:
    double testDelay_;
    dsp::FractionalDelay delay_;
    dsp::LinearRamp level_{1.0f};
    bool bypassed_ = false;
    int pad_ = 0;
};

} // namespace osp::engine
