#include "osp/dsp/util/DelayLine.h"

#include <algorithm>

namespace osp::dsp {

void DelayLine::prepare(int maxDelaySamples)
{
    size_ = std::max(maxDelaySamples, 0) + 1;
    buffer_.assign(static_cast<std::size_t>(size_), 0.0f);
    delay_ = std::clamp(requested_, 0, size_ - 1);
    write_ = 0;
}

void DelayLine::setDelay(int delaySamples) noexcept
{
    requested_ = delaySamples;
    const int maxDelay = size_ > 0 ? size_ - 1 : 0;
    delay_ = std::clamp(delaySamples, 0, maxDelay);
}

void DelayLine::reset() noexcept
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    write_ = 0;
}

void DelayLine::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        std::fill(out, out + numSamples, 0.0f);
        return;
    }

    float* const buf = buffer_.data();
    for (int i = 0; i < numSamples; ++i) {
        // Write first, then read `delay_` samples back, so delay 0 is an exact pass-through and in == out works.
        buf[write_] = in[i];
        int read = write_ - delay_;
        if (read < 0)
            read += size_;
        out[i] = buf[read];
        if (++write_ == size_)
            write_ = 0;
    }
}

} // namespace osp::dsp
