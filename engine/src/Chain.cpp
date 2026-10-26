#include "osp/engine/Chain.h"

namespace osp::engine {

void Chain::prepare(double sampleRate, int maxBlockSize) noexcept
{
    output_.prepare(sampleRate, maxBlockSize);
}

void Chain::reset() noexcept
{
    output_.reset();
}

void Chain::setOutputLevelDb(float levelDb) noexcept
{
    output_.setGainDb(levelDb);
}

void Chain::process(const float* in, float* out, int numSamples) noexcept
{
    output_.process(in, out, numSamples);
}

} // namespace osp::engine
