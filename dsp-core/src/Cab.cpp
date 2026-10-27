#include "osp/dsp/Cab.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace osp::dsp {

namespace {

constexpr int kMaxPartition = PartitionedConvolver::kMaxPartition;

// Exact at centre (1, 1) and at the hard pans (√2, 0): the float trig would land an ulp off, and the default must
// be bit-exact.
void panGains(float pan, float& gl, float& gr) noexcept
{
    if (pan == 0.0f || pan <= -1.0f || pan >= 1.0f) {
        gl = pan == 0.0f ? 1.0f : pan < 0.0f ? std::numbers::sqrt2_v<float> : 0.0f;
        gr = pan == 0.0f ? 1.0f : pan < 0.0f ? 0.0f : std::numbers::sqrt2_v<float>;
        return;
    }
    const float theta = (pan + 1.0f) * (std::numbers::pi_v<float> / 4.0f);
    gl = std::numbers::sqrt2_v<float> * std::cos(theta);
    gr = std::numbers::sqrt2_v<float> * std::sin(theta);
}

// NaN-safe clamp: NaN maps to `lo` (std::clamp passes NaN through, and a NaN target would stick in its ramp).
float clampSafe(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

void convolve(PartitionedConvolver* ir, const float* in, float* out, int n) noexcept
{
    if (ir != nullptr)
        ir->process(in, out, n);
    else
        std::fill(out, out + n, 0.0f);
}

} // namespace

Cab::Cab() noexcept
{
    slots_[0].enabled.reset(1.0f); // parameters.md §3.1: ir1 on, ir2 off
    enableL_.reset(true);
    enableR_.reset(true);
}

void Cab::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(maxBlockSize, 1);
    const int maxDelay = static_cast<int>(std::ceil(kMaxDelayMs * 0.001 * sampleRate));
    int ring = 4;
    while (ring < maxDelay + 3) // the cubic reads one sample beyond the delay, plus the current one
        ring *= 2;
    for (auto& s : slots_) {
        s.swap.prepare(sampleRate, kSwapSeconds);
        s.enabled.prepare(sampleRate, 0.010);
        s.level.prepare(sampleRate, 0.020);
        s.sign.prepare(sampleRate, 0.010);
        s.pan.prepare(sampleRate, 0.020);
        s.delay.prepare(sampleRate, 0.020);
        s.delay.reset(static_cast<float>(double(s.delayMs) * sampleRate / 1000.0));
        s.history.assign(static_cast<std::size_t>(ring), 0.0f);
    }
    enableL_.prepare(sampleRate);
    enableR_.prepare(sampleRate);
    for (auto* buffer : {&mono_, &incoming_, &wetL_, &wetR_})
        buffer->assign(static_cast<std::size_t>(maxBlock_), 0.0f);
    reset();
}

void Cab::reset() noexcept
{
    clock_ = 0;
    for (auto& s : slots_) {
        while (s.incoming != nullptr || s.pending != nullptr)
            completeSwap(s);
        s.phase = slotBase(s);
        s.stale = false;
        if (s.active != nullptr) {
            s.active->setStartPhase(s.phase);
            s.active->reset();
        }
        for (auto* ramp : {&s.enabled, &s.level, &s.sign, &s.pan, &s.delay})
            ramp->reset(ramp->target());
        std::fill(s.history.begin(), s.history.end(), 0.0f);
        s.write = 0;
    }
    enableL_.reset(enableL_.target());
    enableR_.reset(enableR_.target());
}

void Cab::setIr(int slot, PartitionedConvolver* ir) noexcept
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    if (s.incoming == nullptr) {
        startSwap(s, ir);
    } else {
        retire(s.pending); // superseded before it was ever heard
        s.pending = ir;
    }
}

PartitionedConvolver* Cab::popRetired() noexcept
{
    return retiredCount_ > 0 ? retired_[static_cast<std::size_t>(--retiredCount_)] : nullptr;
}

void Cab::retireAll() noexcept
{
    for (auto& s : slots_) {
        for (auto** ir : {&s.active, &s.incoming, &s.pending}) {
            retire(*ir);
            *ir = nullptr;
        }
        s.swap.reset(false);
    }
}

bool Cab::isSwapping(int slot) const noexcept
{
    return slot >= 0 && slot < kNumSlots && slots_[static_cast<std::size_t>(slot)].incoming != nullptr;
}

void Cab::setSlotEnabled(int slot, bool on) noexcept
{
    if (slot >= 0 && slot < kNumSlots)
        slots_[static_cast<std::size_t>(slot)].enabled.setTarget(on ? 1.0f : 0.0f);
}

void Cab::setLevelDb(int slot, float dB) noexcept
{
    if (slot >= 0 && slot < kNumSlots)
        slots_[static_cast<std::size_t>(slot)].level.setTarget(std::pow(10.0f, clampSafe(dB, -60.0f, 6.0f) / 20.0f));
}

void Cab::setPanPct(int slot, float pct) noexcept
{
    if (slot >= 0 && slot < kNumSlots)
        slots_[static_cast<std::size_t>(slot)].pan.setTarget(clampSafe(pct, -100.0f, 100.0f) / 100.0f);
}

void Cab::setInvert(int slot, bool on) noexcept
{
    if (slot >= 0 && slot < kNumSlots)
        slots_[static_cast<std::size_t>(slot)].sign.setTarget(on ? -1.0f : 1.0f);
}

void Cab::setDelayMs(int slot, float ms) noexcept
{
    if (slot < 0 || slot >= kNumSlots)
        return;
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    s.delayMs = clampSafe(ms, 0.0f, kMaxDelayMs);
    s.delay.setTarget(static_cast<float>(double(s.delayMs) * sampleRate_ / 1000.0));
}

void Cab::retire(PartitionedConvolver* ir) noexcept
{
    if (ir == nullptr)
        return;
    assert(retiredCount_ < kRetireCapacity && "drain popRetired() after every setIr() and process()");
    if (retiredCount_ < kRetireCapacity)
        retired_[static_cast<std::size_t>(retiredCount_++)] = ir;
}

void Cab::startSwap(Slot& s, PartitionedConvolver* ir, int ahead) noexcept
{
    if (!isPrepared()) { // nothing is playing yet: install without a fade
        retire(s.active);
        s.active = ir;
        return;
    }
    const int base = slotBase(s);
    s.phase = base + (s.phase - base + kMaxPartition / 4) % (kMaxPartition / 2);
    if (ir != nullptr)
        ir->setStartPhase(phaseNow(s.phase, ahead)); // ir is cold: just prepared by the worker
    s.incoming = ir;
    s.swap.reset(false);
    s.swap.setTarget(true);
}

void Cab::completeSwap(Slot& s, int ahead) noexcept
{
    retire(s.active);
    s.active = s.incoming;
    s.incoming = nullptr;
    s.swap.reset(false);
    if (s.pending != nullptr) {
        PartitionedConvolver* const next = s.pending;
        s.pending = nullptr;
        startSwap(s, next, ahead);
    }
}

int Cab::slotBase(const Slot& s) const noexcept
{
    return static_cast<int>(&s - slots_.data()) * (kMaxPartition / 2);
}

int Cab::phaseNow(int phase, int ahead) const noexcept
{
    const unsigned t = clock_ + static_cast<unsigned>(ahead) + static_cast<unsigned>(phase);
    return static_cast<int>(t % static_cast<unsigned>(kMaxPartition));
}

void Cab::renderSlot(Slot& s, const float* in, float* outL, float* outR, int n) noexcept
{
    if (!s.enabled.isRamping() && s.enabled.current() == 0.0f) {
        s.stale = true; // slot off: skipped
        return;
    }
    if (s.stale) { // back after a skip: drop the stale input and re-phase (one-off clear, RT-safe)
        for (PartitionedConvolver* ir : {s.active, s.incoming})
            if (ir != nullptr) {
                ir->setStartPhase(phaseNow(s.phase));
                ir->reset();
            }
        std::fill(s.history.begin(), s.history.end(), 0.0f);
        s.stale = false;
    }

    float* const m = mono_.data();
    convolve(s.active, in, m, n);
    if (s.incoming != nullptr) {
        convolve(s.incoming, in, incoming_.data(), n);
        s.swap.process(m, incoming_.data(), m, n);
        if (!s.swap.isFading())
            completeSwap(s, n); // a promoted pending IR starts with the next chunk
    }

    const int mask = static_cast<int>(s.history.size()) - 1;
    const auto past = [&](int k) noexcept { return s.history[static_cast<std::size_t>((s.write - k) & mask)]; };
    float gl = 0.0f, gr = 0.0f;
    panGains(s.pan.current(), gl, gr);
    for (int i = 0; i < n; ++i) {
        s.history[static_cast<std::size_t>(s.write)] = m[i];
        const float d = s.delay.next();
        const int di = static_cast<int>(d);
        const float t = d - static_cast<float>(di);
        float y = past(di);
        if (t > 0.0f) {
            const float p1 = y, p2 = past(di + 1);
            if (di == 0) {
                y = p1 + t * (p2 - p1);
            } else {
                const float p0 = past(di - 1), p3 = past(di + 2);
                y = p1 +
                    0.5f * t *
                        (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
            }
        }
        s.write = (s.write + 1) & mask;

        if (s.pan.isRamping())
            panGains(s.pan.next(), gl, gr);
        const float g = s.enabled.next() * s.level.next() * s.sign.next();
        outL[i] += g * gl * y;
        outR[i] += g * gr * y;
    }
}

void Cab::process(const float* in, float* outL, float* outR, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    if (bypassed_) {
        std::copy(in, in + numSamples, outR);
        if (in != outL)
            std::copy(in, in + numSamples, outL);
        return;
    }
    for (int done = 0; done < numSamples; done += maxBlock_) {
        const int n = std::min(maxBlock_, numSamples - done);
        const float* const x = in + done;
        if (!enableL_.isSettledOnA()) { // a settled bypass skips the wet path (Crossfader leaves b unread)
            std::fill(wetL_.begin(), wetL_.begin() + n, 0.0f);
            std::fill(wetR_.begin(), wetR_.begin() + n, 0.0f);
            for (auto& s : slots_)
                renderSlot(s, x, wetL_.data(), wetR_.data(), n);
        } else {
            for (auto& s : slots_)
                s.stale = true;
        }
        // Right first: `in` may alias outL.
        enableR_.process(x, wetR_.data(), outR + done, n);
        enableL_.process(x, wetL_.data(), outL + done, n);
        clock_ += static_cast<unsigned>(n);
    }
}

} // namespace osp::dsp
