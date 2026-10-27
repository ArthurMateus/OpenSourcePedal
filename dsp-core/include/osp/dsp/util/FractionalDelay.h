#pragma once

// Fractional delay line for modulated taps: the delay block's read pointer (`delay.timeMs` glide, wow/flutter, BBD
// clock), and later chorus/flanger/doubler (time-based-effects.md §1.4). Pure C++20: no JUCE, no OS headers.
// prepare() is the only call that allocates; everything else is RT-safe and bounded.
//
// Order per sample: read(d) first, then push(x). read(d) returns x[n − d], where x[n] is the sample about to be
// pushed, so d >= 1 never depends on the current input: a feedback loop can read, process and push.
//
// Interpolation: Catmull-Rom (4-point cubic Hermite) between the integer taps for d >= 2, linear for 1 <= d < 2
// (the newer neighbour x[n − d + 1] is the not-yet-pushed sample there). Both are exact at integer delays. Sources:
// time-based-effects.md §1.4 (Catmull-Rom for modulated taps, as in Signalsmith `dsp` delay.h, MIT); P. Bourke,
// "Interpolation methods", 1999, https://paulbourke.net/miscellaneous/interpolation/ (accessed 2026-10-26).

#include <vector>

namespace osp::dsp {

class FractionalDelay {
public:
    // Non-RT. Allocates and zero-fills room for delays up to maxDelaySamples (>= 1).
    void prepare(int maxDelaySamples)
    {
        maxDelay_ = maxDelaySamples > 1 ? maxDelaySamples : 1;
        int size = 4;
        while (size < maxDelay_ + 3) // the cubic reads two taps beyond floor(d), plus the write slot
            size *= 2;
        buffer_.assign(static_cast<std::size_t>(size), 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }

    // RT-safe. Clears the history, so two renders after reset() are bit-identical.
    void reset() noexcept
    {
        for (auto& v : buffer_)
            v = 0.0f;
        write_ = 0;
    }

    // RT-safe. A no-op before prepare().
    void push(float x) noexcept
    {
        if (buffer_.empty())
            return;
        buffer_[static_cast<std::size_t>(write_)] = x;
        write_ = (write_ + 1) & mask_;
    }

    // RT-safe. x[n − d] for d clamped to [1, getMaxDelay()] (NaN → 1). Returns 0 before prepare().
    float read(float d) const noexcept
    {
        if (buffer_.empty())
            return 0.0f;
        const float maxD = static_cast<float>(maxDelay_);
        d = d > 1.0f ? (d < maxD ? d : maxD) : 1.0f;
        const int di = static_cast<int>(d);
        const float t = d - static_cast<float>(di);
        const float p1 = at(di);
        if (t == 0.0f)
            return p1;
        const float p2 = at(di + 1);
        if (di == 1)
            return p1 + t * (p2 - p1);
        const float p0 = at(di - 1), p3 = at(di + 2);
        return p1 +
               0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    }

    int getMaxDelay() const noexcept { return maxDelay_; }
    bool isPrepared() const noexcept { return !buffer_.empty(); }

private:
    // x[n − k], k >= 1 (x[n] is the next sample to be pushed).
    float at(int k) const noexcept { return buffer_[static_cast<std::size_t>((write_ - k) & mask_)]; }

    std::vector<float> buffer_;
    int mask_ = 0;
    int write_ = 0;
    int maxDelay_ = 1;
};

} // namespace osp::dsp
