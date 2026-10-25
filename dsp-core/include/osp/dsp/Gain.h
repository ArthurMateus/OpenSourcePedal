#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).

namespace osp::dsp {

// Mono gain with a linear ramp on every gain change. The reference shape for every dsp-core block:
// prepare() is the only call that may allocate; process() is noexcept, bounded and safe for any n >= 0.
class Gain {
public:
    // Non-RT. Ramp length is fixed here, in samples at the given rate.
    void prepare(double sampleRate, int maxBlockSize, double rampSeconds = 0.02) noexcept;
    // RT-safe. Jumps to the target gain and clears the ramp, so two renders after reset() are bit-identical.
    void reset() noexcept;
    // RT-safe. Called once per block with the parameter value read from an atomic.
    void setGainDb(float gainDb) noexcept;
    // RT-safe. in == out is allowed. Before prepare() it zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return rampLength_ > 0; }

private:
    int rampLength_ = 0;
    int rampRemaining_ = 0;
    float current_ = 1.0f;
    float target_ = 1.0f;
    float step_ = 0.0f;
};

} // namespace osp::dsp
