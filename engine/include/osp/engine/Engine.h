#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3). Threading rules: ADR-0004, docs/spec/threading.md.

#include "osp/rt/AudioThread.h"

#include "osp/engine/Graph.h"
#include "osp/engine/Params.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace osp::engine {

// Host-facing wrapper: the parameter store, the v1 signal graph (ADR-0009) and block splitting. The host input is
// mono (channel 1, REQ-207); the graph's stereo output goes to the first two host channels.
class Engine {
public:
    static constexpr int kMaxChannels = 2;

    // Control thread. Builds the graph (allocates).
    Engine();
    ~Engine();

    // Control thread, before prepare(). The host owns the atomic (APVTS raw value, in dB); ADR-0004 rule 3.
    void setOutputLevelParameter(const std::atomic<float>* levelDb) noexcept;

    // Control thread. The parameter store the graph reads once per block (ADR-0009 D8): bind host atomics here, or
    // write preset-only values.
    ParamStore& parameters() noexcept { return params_; }
    const ParamStore& parameters() const noexcept { return params_; }
    // Control thread, audio stopped (tests, the future controller): the graph itself.
    Graph& graph() noexcept { return *graph_; }
    // The engine-owned "no cab" unit impulse of a strip (ir1 until an IR is installed). A convolver the graph retires
    // that equals one of these belongs to the engine: whoever installs IRs must not delete it. Any thread.
    dsp::PartitionedConvolver* noCabIr(int channel) noexcept { return noCab_[static_cast<std::size_t>(channel)].get(); }

    // Control thread, audio stopped (JUCE prepareToPlay contract). The only call that may allocate.
    void prepare(double sampleRate, int maxBlockSize);

    // Audio thread. Clears internal state; repeat renders after reset() are bit-identical.
    void reset() noexcept OSP_RT_NONBLOCKING;

    // Audio thread. In-place on `numChannels` (<= kMaxChannels) buffers, 0 <= numSamples <= getMaxBlockSize().
    // Before prepare() the buffers are zero-filled.
    void process(float* const* channels, int numChannels, int numSamples) noexcept OSP_RT_NONBLOCKING;

    // L_total = round(sum of the measured delays), rounded once (ADR-0009 D4). The app reports it with
    // setLatencySamples after prepare().
    int getLatencySamples() const noexcept OSP_RT_NONBLOCKING { return graph_->getLatencySamples(); }
    int getMaxBlockSize() const noexcept OSP_RT_NONBLOCKING { return maxBlockSize_; }

    // Telemetry for the editor's meters (threading.md §3.5). Any thread: the peak |sample| of the host input (channel
    // 1) or of the engine output (all channels) since the last call, then cleared. The audio thread keeps the larger of
    // the stored and the new block peak; a reset that lands between its load and store can drop one block's peak,
    // which a meter does not show.
    float takeInputPeak() noexcept { return inputPeak_.exchange(0.0f, std::memory_order_relaxed); }
    float takeOutputPeak() noexcept { return outputPeak_.exchange(0.0f, std::memory_order_relaxed); }

private:
    ParamStore params_;
    std::unique_ptr<Graph> graph_;
    // Until a cab IR is loaded (preset loader, ADR-0004 §4), ir1 of every strip is a unit impulse: no cab. The
    // engine owns these; anything the graph retires that is not one of them belongs to whoever installed it.
    std::array<std::unique_ptr<dsp::PartitionedConvolver>, Graph::kNumChannels> noCab_;
    std::vector<float> right_; // the graph's right output when the host has one channel
    int maxBlockSize_ = 0;
    std::atomic<float> inputPeak_{0.0f}, outputPeak_{0.0f};
    bool noCabInstalled_ = false;
};

// Audio thread. Splits a host block of any length into chunks of at most engine.getMaxBlockSize() (ADR-0004 rule 8,
// pluginval level 8). The app calls this instead of Engine::process().
void processChunked(Engine& engine,
                    float* const* channels,
                    int numChannels,
                    int numSamples) noexcept OSP_RT_NONBLOCKING;

} // namespace osp::engine
