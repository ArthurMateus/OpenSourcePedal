#include "CabIrInstaller.h"

#include <algorithm>

namespace osp::app {

bool CabIrInstaller::install(int channel, int slot, std::span<const float> ir)
{
    collect();
    auto convolver = std::make_unique<osp::dsp::PartitionedConvolver>();
    convolver->prepare(ir.data(), static_cast<int>(ir.size()));
    if (!pending_.push({channel, slot, convolver.get()}))
        return false;
    live_.push_back(std::move(convolver));
    return true;
}

bool CabIrInstaller::installNone(osp::engine::Engine& engine, int channel, int slot)
{
    collect();
    return pending_.push({channel, slot, slot == 0 ? engine.noCabIr(channel) : nullptr});
}

void CabIrInstaller::collect()
{
    osp::dsp::PartitionedConvolver* gone = nullptr;
    while (retired_.pop(gone))
        std::erase_if(live_, [gone](const auto& c) { return c.get() == gone; });
}

void CabIrInstaller::pump(osp::engine::Engine& engine) noexcept OSP_RT_NONBLOCKING
{
    Request r;
    while (pending_.pop(r)) {
        engine.graph().setCabIr(r.channel, r.slot, r.ir);
        drain(engine);
    }
}

void CabIrInstaller::drain(osp::engine::Engine& engine) noexcept OSP_RT_NONBLOCKING
{
    auto& graph = engine.graph();
    while (auto* ir = graph.popRetired()) {
        bool engineOwned = false;
        for (int ch = 0; ch < osp::engine::Graph::kNumChannels; ++ch)
            engineOwned = engineOwned || ir == engine.noCabIr(ch);
        if (!engineOwned)
            retired_.push(
                ir); // full only if collect() stopped being called: the IR then stays alive (no leak of audio)
    }
}

} // namespace osp::app
