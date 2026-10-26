#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3). Threading rules: ADR-0004, docs/spec/threading.md.

#include "osp/engine/Chain.h"

#include <array>
#include <atomic>

namespace osp::engine {

// Host-facing wrapper: one Chain per channel (the same Chain osp-render and osp-bench drive), the host-owned
// parameter atomics, and block splitting. The signal graph grows inside Chain.
class Engine {
public:
    static constexpr int kMaxChannels = 2;

    // Control thread, before prepare(). The host owns the atomic (APVTS raw value, in dB); ADR-0004 rule 3.
    void setOutputLevelParameter(const std::atomic<float>* levelDb) noexcept { outputLevelDb_ = levelDb; }

    // Control thread, audio stopped (JUCE prepareToPlay contract). The only call that may allocate.
    void prepare(double sampleRate, int maxBlockSize);

    // Audio thread. Clears internal state; repeat renders after reset() are bit-identical.
    void reset() noexcept;

    // Audio thread. In-place on `numChannels` (<= kMaxChannels) buffers, 0 <= numSamples <= getMaxBlockSize().
    // Before prepare() the buffers are zero-filled.
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return chains_[0].getLatencySamples(); }
    int getMaxBlockSize() const noexcept { return maxBlockSize_; }

private:
    const std::atomic<float>* outputLevelDb_ = nullptr;
    std::array<Chain, kMaxChannels> chains_{};
    int maxBlockSize_ = 0;
};

// Audio thread. Splits a host block of any length into chunks of at most engine.getMaxBlockSize() (ADR-0004 rule 8,
// pluginval level 8). The app calls this instead of Engine::process().
void processChunked(Engine& engine, float* const* channels, int numChannels, int numSamples) noexcept;

} // namespace osp::engine
