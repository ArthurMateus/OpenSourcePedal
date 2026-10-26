#include "osp/engine/Engine.h"

#include <algorithm>

namespace osp::engine {

void Engine::prepare(double sampleRate, int maxBlockSize) {
  maxBlockSize_ = std::max(1, maxBlockSize);
  for (auto &gain : gains_) {
    if (gainDb_ != nullptr)
      gain.setGainDb(gainDb_->load(std::memory_order_relaxed));
    gain.prepare(sampleRate, maxBlockSize_);
  }
}

void Engine::reset() noexcept {
  for (auto &gain : gains_)
    gain.reset();
}

void Engine::process(float *const *channels, int numChannels,
                     int numSamples) noexcept {
  numChannels = std::min(numChannels, kMaxChannels);
  if (numSamples <= 0 || numChannels <= 0)
    return;

  // Parameters are read once per block and smoothed per sample inside dsp-core
  // (ADR-0004 rule 3).
  if (gainDb_ != nullptr) {
    const float gainDb = gainDb_->load(std::memory_order_relaxed);
    for (int ch = 0; ch < numChannels; ++ch)
      gains_[static_cast<std::size_t>(ch)].setGainDb(gainDb);
  }
  for (int ch = 0; ch < numChannels; ++ch)
    gains_[static_cast<std::size_t>(ch)].process(channels[ch], channels[ch],
                                                 numSamples);
}

void processChunked(Engine &engine, float *const *channels, int numChannels,
                    int numSamples) noexcept {
  numChannels = std::min(numChannels, Engine::kMaxChannels);
  const int chunk = std::max(1, engine.getMaxBlockSize());
  std::array<float *, Engine::kMaxChannels> offset{};
  for (int start = 0; start < numSamples; start += chunk) {
    for (int ch = 0; ch < numChannels; ++ch)
      offset[static_cast<std::size_t>(ch)] = channels[ch] + start;
    engine.process(offset.data(), numChannels,
                   std::min(chunk, numSamples - start));
  }
}

} // namespace osp::engine
