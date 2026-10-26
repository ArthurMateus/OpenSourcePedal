#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3).

#include "osp/dsp/Gain.h"

namespace osp::engine {

// Minimal mono chain: input -> output gain. It is the pass-through chain that osp-render and osp-bench drive until
// the signal graph (signal-flow.md) replaces its internals. The interface (prepare/reset/process) stays.
// Same contract as the dsp-core blocks: prepare() is non-RT; everything else is RT-safe and noexcept.
class Chain {
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;
    // RT-safe. output.levelDb (parameters.md §3.6), called once per block.
    void setOutputLevelDb(float levelDb) noexcept;
    // RT-safe. in == out is allowed. At 0 dB the output is bit-identical to the input.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return output_.getLatencySamples(); }

private:
    dsp::Gain output_;
};

} // namespace osp::engine
