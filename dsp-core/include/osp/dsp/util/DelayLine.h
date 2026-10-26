#pragma once

// Fixed integer delay line for latency padding and dry-path alignment (signal-flow.md §4–5, ADR-0005 D4).
// Pure C++20. prepare() is the only call that allocates; everything else is RT-safe and bounded.

#include <vector>

namespace osp::dsp {

class DelayLine {
public:
    // Non-RT. Allocates and zero-fills (pre-touches, realtime-rules.md RT-12) room for up to maxDelaySamples.
    void prepare(int maxDelaySamples);

    // RT-safe. Clamped to [0, maxDelaySamples]; a value set before prepare() is kept and clamped there. Latency
    // padding sets it once; changing it while running is a jump (no interpolation), by design.
    void setDelay(int delaySamples) noexcept;

    // RT-safe. Clears the history, so two renders after reset() are bit-identical.
    void reset() noexcept;

    // RT-safe. in == out is allowed. Before prepare() it zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getDelay() const noexcept { return delay_; }
    int getLatencySamples() const noexcept { return delay_; }
    int getMaxDelay() const noexcept { return size_ - 1; }
    bool isPrepared() const noexcept { return size_ > 0; }

private:
    std::vector<float> buffer_;
    int size_ = 0; // maxDelay + 1 once prepared
    int write_ = 0;
    int delay_ = 0;
    int requested_ = 0; // last setDelay() value, re-clamped by prepare()
};

} // namespace osp::dsp
