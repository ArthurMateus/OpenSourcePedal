#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3). Threading rules: ADR-0004, docs/spec/threading.md.

#include "osp/dsp/Gain.h"

#include <array>
#include <atomic>

namespace osp::engine {

// v1 pass-through engine: one Gain per channel. Grows into the signal graph in later tickets.
class Engine {
public:
    static constexpr int kMaxChannels = 2;

    // Control thread, before prepare(). The host owns the atomic (APVTS raw value, in dB); ADR-0004 rule 3.
    void setGainParameter(const std::atomic<float>* gainDb) noexcept { gainDb_ = gainDb; }

    // Control thread, audio stopped (JUCE prepareToPlay contract). The only call that may allocate.
    void prepare(double sampleRate, int maxBlockSize);

    // Audio thread. Clears internal state; repeat renders after reset() are bit-identical.
    void reset() noexcept;

    // Audio thread. In-place on `numChannels` (<= kMaxChannels) buffers, 0 <= numSamples <= getMaxBlockSize().
    // Before prepare() the buffers are zero-filled.
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    int getMaxBlockSize() const noexcept { return maxBlockSize_; }

private:
    const std::atomic<float>* gainDb_ = nullptr;
    std::array<osp::dsp::Gain, kMaxChannels> gains_{};
    int maxBlockSize_ = 0;
};

// Audio thread. Splits a host block of any length into chunks of at most engine.getMaxBlockSize() (ADR-0004 rule 8,
// pluginval level 8). The app calls this instead of Engine::process().
void processChunked(Engine& engine, float* const* channels, int numChannels, int numSamples) noexcept;

} // namespace osp::engine
