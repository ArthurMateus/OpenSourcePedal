#pragma once

// NAM capture behind IAmpModel (ADR-0002 D1-D4, D9; ADR-0011 D2, slice P5-3 / #169). The runtime is
// NeuralAmpModelerCore v0.6.0 (MIT, https://github.com/sdatkinson/NeuralAmpModelerCore, commit 0b3d3c9), linked
// PRIVATE into osp_engine. This header names NAM types only by forward declaration, so tests can include it without
// NAM's include directories (REQ-NAM-009 covers engine/include/; this file is src-only).
//
// - Rate: v1 runs at the model's training rate only (48 kHz when the file has none, ADR-0002 D4). At that rate the
//   processing delay is 0 samples (ADR-0011 A2: slot delay = processing delay). Other host rates throw from
//   prepare() until the resampler lands (#170).
// - reset() (ADR-0002 D2 open item, decided here): NAM core's own public state restore, chosen per model at load.
//   * prewarmCache: WaveNet (A1 and the A2 fast path), slimmable WaveNet and SlimmableContainer whose active
//     network is one of those. After the first prewarm, NAM core v0.6.0 caches each convolution's steady silence
//     column and DSP::prewarm() rebuilds every history from it (Conv1D::PrewarmFromCache, RingBuffer::FillWithSample,
//     A2FastModel::PrewarmFromCache): a copy of O(state), no model compute and no allocation.
//   * lstmCells: LSTM. Its cells (nam::lstm::LSTM::_layers, a public member) are copy-assigned from a snapshot
//     taken after the prewarm; Eigen assignment between equal sizes does not allocate.
//   A second prewarmed instance (the ADR's fallback) was rejected: swapping it in consumes the snapshot, and making
//   the displaced instance pristine again needs a model run (GetPrewarmSamples() of silence) or a worker round trip,
//   so a second reset() could not be bit-identical. Models with neither path (a WaveNet with a condition DSP,
//   ConvNet, Linear, a container whose active network is an LSTM) are rejected at load in v1 with a clear message.
// - Activations: NAM core's fast_tanh is enabled process-wide before the first model is built, as the reference
//   plugin does (NeuralAmpModeler.cpp constructor). Measured on A1-Standard at 48 kHz / 64 (osp-bench, Ryzen 5 9600X):
//   18.4% of a core with std::tanh, 7.7% with fast_tanh (#169).
// - process(): the input is copied into a preallocated scratch with non-finite or |x| > 1e6 samples replaced by 0
//   (a NaN would otherwise stay in the convolution histories), then NAM core's process() runs on it.

#include "osp/engine/AmpModel.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace nam {
class DSP;
} // namespace nam

namespace osp::engine {

class NamAmpModel final : public IAmpModel {
public:
    enum class ResetPath { prewarmCache, lstmCells };

    NamAmpModel(std::unique_ptr<nam::DSP> model, AmpModelInfo info, ResetPath path);
    ~NamAmpModel() override;

    // Throws ModelLoadError when hostSampleRate is not the model's training rate (#170). `quality` is ignored: NAM
    // quality is a load option (ADR-0002 D9).
    void prepare(double hostSampleRate, int maxBlockSize, dsp::OversamplingQuality quality) override;
    void process(const float* in, float* out, int numSamples) noexcept OSP_RT_NONBLOCKING override;
    void reset() noexcept OSP_RT_NONBLOCKING override;
    double getMeasuredDelaySamples() const noexcept override { return 0.0; }
    int getLatencySamples() const noexcept override { return 0; }
    const AmpModelInfo& info() const noexcept override { return info_; }

    ResetPath resetPath() const noexcept { return path_; }
    bool isPrepared() const noexcept { return maxBlock_ > 0; }

private:
    struct LstmSnapshot;

    std::unique_ptr<nam::DSP> model_;
    AmpModelInfo info_;
    ResetPath path_;
    std::unique_ptr<LstmSnapshot> lstm_;
    std::vector<float> scratch_;
    int maxBlock_ = 0;
};

// Test support for REQ-NAM-003: NAM core used directly: get_dsp(path), Reset(fs, block) with NAM's default prewarm,
// then process() block by block, with the same fast_tanh setting. No sanitising and no OSP code in between.
std::vector<float>
renderWithNamCore(const std::filesystem::path& path, double sampleRate, int block, const std::vector<float>& input);

} // namespace osp::engine
