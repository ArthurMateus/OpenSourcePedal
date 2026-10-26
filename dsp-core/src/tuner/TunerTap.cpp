#include "osp/dsp/tuner/TunerTap.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

static_assert(std::atomic<float>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free,
              "RT-6: the tuner hand-off must be lock-free");
static_assert((TunerTap::kRingSize & (TunerTap::kRingSize - 1)) == 0, "the ring index is masked");

void TunerTap::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    factor_ = std::max(1, static_cast<int>(std::lround(sampleRate / kTargetRate)));
    // 4th-order Butterworth = two 2nd-order sections with Q = 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8)).
    lp1_ = svfDesign(SvfType::lowPass, sampleRate, kLowPassHz, 0.54119610014619698, 0.0);
    lp2_ = svfDesign(SvfType::lowPass, sampleRate, kLowPassHz, 1.30656296487637652, 0.0);
    mute_.prepare(sampleRate, kMuteSeconds);
    reset();
}

void TunerTap::reset() noexcept
{
    s1_ = {};
    s2_ = {};
    phase_ = 0;
    produced_ = 0;
    for (auto& v : ring_)
        v.store(0.0f, std::memory_order_relaxed);
    written_.store(0, std::memory_order_release);
    mute_.reset(mute_.target());
}

void TunerTap::push(const float* in, int numSamples) noexcept
{
    if (!isPrepared() || in == nullptr || numSamples <= 0)
        return;
    for (int i = 0; i < numSamples; ++i) {
        const float x = std::isfinite(in[i]) ? in[i] : 0.0f;
        const float y = s2_.tick(lp2_, s1_.tick(lp1_, x));
        if (++phase_ >= factor_) {
            phase_ = 0;
            ring_[produced_ & (kRingSize - 1)].store(y, std::memory_order_relaxed);
            ++produced_;
        }
    }
    written_.store(produced_, std::memory_order_release); // publishes the samples stored above
}

void TunerTap::applyMute(float* io, int numSamples) noexcept
{
    if (io == nullptr || numSamples <= 0 || (!mute_.isRamping() && mute_.current() == 1.0f))
        return; // unmuted and idle: bit-exact
    for (int i = 0; i < numSamples; ++i)
        io[i] *= mute_.next();
}

bool TunerTap::readLatest(float* dst, int n) const noexcept
{
    if (dst == nullptr || n <= 0 || n > kRingSize / 2)
        return false;
    const std::uint32_t end = written_.load(std::memory_order_acquire);
    if (end < static_cast<std::uint32_t>(n))
        return false; // not enough samples since reset() (also guards the first wrap of the counter)
    const std::uint32_t begin = end - static_cast<std::uint32_t>(n);
    for (int i = 0; i < n; ++i)
        dst[i] = ring_[(begin + static_cast<std::uint32_t>(i)) & (kRingSize - 1)].load(std::memory_order_relaxed);
    // Seqlock-style check: if the producer has written more than the ring holds past `begin`, part of dst is newer
    // than the rest. Unsigned difference, so the 32-bit counter may wrap.
    const std::uint32_t now = written_.load(std::memory_order_acquire);
    return now - begin <= static_cast<std::uint32_t>(kRingSize);
}

} // namespace osp::dsp
