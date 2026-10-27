#include "osp/engine/Engine.h"

#include <algorithm>

namespace osp::engine {

Engine::Engine() : graph_(std::make_unique<Graph>(params_))
{
    const float unit = 1.0f;
    for (auto& ir : noCab_) {
        ir = std::make_unique<dsp::PartitionedConvolver>();
        ir->prepare(&unit, 1);
    }
}

Engine::~Engine()
{
    graph_.reset(); // hands every installed convolver back before noCab_ goes
}

void Engine::setOutputLevelParameter(const std::atomic<float>* levelDb) noexcept
{
    params_.bind(findParam("output.levelDb"), levelDb);
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    maxBlockSize_ = std::max(1, maxBlockSize);
    right_.assign(static_cast<std::size_t>(maxBlockSize_), 0.0f);
    graph_->prepare(sampleRate, maxBlockSize_); // the installed IRs are kept (Cab::prepare)
    if (!noCabInstalled_) {
        for (int ch = 0; ch < Graph::kNumChannels; ++ch)
            graph_->setCabIr(ch, 0, noCab_[static_cast<std::size_t>(ch)].get());
        noCabInstalled_ = true;
    }
    graph_->reset(); // the IRs are in place from the first sample, with no fade
    while (graph_->popRetired() != nullptr) { // nothing was installed before: nothing of anyone else's comes back
    }
}

void Engine::reset() noexcept
{
    graph_->reset();
}

void Engine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min(numChannels, kMaxChannels);
    if (numSamples <= 0 || numChannels <= 0)
        return;
    if (maxBlockSize_ == 0) {
        for (int ch = 0; ch < numChannels; ++ch)
            std::fill_n(channels[ch], numSamples, 0.0f);
        return;
    }
    // Mono in from channel 1 (REQ-207 default); the graph reads its input before writing outL, so in place is fine.
    float* const right = numChannels > 1 ? channels[1] : right_.data();
    graph_->process(channels[0], channels[0], right, numSamples);
}

void processChunked(Engine& engine, float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min(numChannels, Engine::kMaxChannels);
    const int chunk = std::max(1, engine.getMaxBlockSize());
    std::array<float*, Engine::kMaxChannels> offset{};
    for (int start = 0; start < numSamples; start += chunk) {
        for (int ch = 0; ch < numChannels; ++ch)
            offset[static_cast<std::size_t>(ch)] = channels[ch] + start;
        engine.process(offset.data(), numChannels, std::min(chunk, numSamples - start));
    }
}

} // namespace osp::engine
