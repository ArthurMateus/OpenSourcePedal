#include "osp/dsp/Reverb.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace osp::dsp {
namespace {

int engineFor(Reverb::Mode m) noexcept
{
    switch (m) {
    case Reverb::Mode::Room:
        return 0;
    case Reverb::Mode::Plate:
        return 2;
    case Reverb::Mode::Spring:
        return 3;
    case Reverb::Mode::Shimmer:
        return 4;
    case Reverb::Mode::Hall:
    default:
        return 1;
    }
}

// NaN-safe clamp: a NaN host value maps to `lo` (std::clamp passes NaN through).
float clampSafe(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

float tptG(float hz, double fs) noexcept
{
    const double g = std::tan(std::numbers::pi * hz / fs);
    return static_cast<float>(g / (1.0 + g));
}

} // namespace

void Reverb::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    engines_[0].prepare(sampleRate, FdnTank::kRoom);
    engines_[1].prepare(sampleRate, FdnTank::kHall);
    plate_.prepare(sampleRate);
    spring_.prepare(sampleRate);
    shimmer_.prepare(sampleRate);
    const auto preLen = static_cast<std::size_t>(0.200 * sampleRate) + 4;
    for (auto& p : pre_)
        p.assign(preLen, 0.0f);
    for (auto& s : scratch_)
        s.assign(static_cast<std::size_t>(std::max(maxBlockSize, 1)), 0.0f);
    preDelay_.prepare(sampleRate, 0.080);
    lowCut_.prepare(sampleRate, 0.020);
    highCut_.prepare(sampleRate, 0.020);
    modeFade_.prepare(sampleRate, 0.050);
    mix_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, Crossfader::kBlockEnableSeconds);
    hard_.prepare(sampleRate);
    setParameters(params_);
    reset();
}

void Reverb::reset() noexcept
{
    for (int e = 0; e < kNumEngines; ++e)
        resetEngine(e);
    for (auto& p : pre_)
        std::fill(p.begin(), p.end(), 0.0f);
    preWrite_ = 0;
    hp_ = {};
    lp_ = {};
    active_ = previous_ = pending_;
    for (auto* s : {&preDelay_, &lowCut_, &highCut_})
        s->reset(s->target());
    for (auto* r : {&modeFade_, &mix_, &enabled_})
        r->reset(r->target());
    modeFade_.reset(1.0f);
    hard_.reset(hard_.target());
    cachedLow_ = cachedHigh_ = -1.0f;
}

void Reverb::setParameters(const Params& p) noexcept
{
    params_.decayS = clampSafe(p.decayS, 0.3f, 20.0f);
    params_.preDelayMs = clampSafe(p.preDelayMs, 0.0f, 200.0f);
    params_.mixPct = clampSafe(p.mixPct, 0.0f, 100.0f);
    params_.lowCutHz = clampSafe(p.lowCutHz, 50.0f, 700.0f);
    params_.highCutHz = clampSafe(p.highCutHz, 1000.0f, 10000.0f);
    params_.modPct = clampSafe(p.modPct, 0.0f, 100.0f);
    params_.shimmerPct = clampSafe(p.shimmerPct, 0.0f, 100.0f);
    params_.shimmerInterval = p.shimmerInterval;
    params_.freeze = p.freeze;
    for (auto& e : engines_) {
        e.setDecaySeconds(params_.decayS);
        e.setModulation(params_.modPct / 100.0f);
        e.setFreeze(params_.freeze);
    }
    plate_.setDecaySeconds(params_.decayS);
    plate_.setModulation(params_.modPct / 100.0f);
    plate_.setFreeze(params_.freeze);
    spring_.setDecaySeconds(params_.decayS);
    spring_.setModulation(params_.modPct / 100.0f);
    spring_.setFreeze(params_.freeze);
    shimmer_.setDecaySeconds(params_.decayS);
    shimmer_.setModulation(params_.modPct / 100.0f);
    shimmer_.setFreeze(params_.freeze);
    shimmer_.setShimmer(params_.shimmerPct / 100.0f);
    shimmer_.setInterval(params_.shimmerInterval);
    preDelay_.setTarget(static_cast<float>(params_.preDelayMs * 1e-3 * sampleRate_));
    lowCut_.setTarget(params_.lowCutHz);
    highCut_.setTarget(params_.highCutHz);
    mix_.setTarget(params_.mixPct / 100.0f);

    params_.mode = p.mode;
    pending_ = engineFor(p.mode);
    if (sampleRate_ <= 0.0)
        active_ = previous_ = pending_; // not prepared yet: no fade
}

void Reverb::startPendingModeFade() noexcept
{
    // One xf-50 fade at a time, started at a block boundary. A mode change during a fade waits for it to end:
    // restarting would cold-reset the engine that is still audible as the outgoing one (the #91 review's bug class).
    if (modeFade_.isRamping() || pending_ == active_)
        return;
    previous_ = active_;
    active_ = pending_;
    resetEngine(active_); // bounded memset of a prepared buffer (RT-7)
    modeFade_.reset(0.0f);
    modeFade_.setTarget(1.0f);
}

void Reverb::resetEngine(int engine) noexcept
{
    switch (engine) {
    case 2:
        plate_.reset();
        break;
    case 3:
        spring_.reset();
        break;
    case 4:
        shimmer_.reset();
        break;
    default:
        engines_[static_cast<std::size_t>(engine)].reset();
    }
}

void Reverb::processEngine(int engine, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    switch (engine) {
    case 2:
        plate_.process(inL, inR, outL, outR, n);
        break;
    case 3:
        spring_.process(inL, inR, outL, outR, n);
        break;
    case 4:
        shimmer_.process(inL, inR, outL, outR, n);
        break;
    default:
        engines_[static_cast<std::size_t>(engine)].process(inL, inR, outL, outR, n);
    }
}

void Reverb::setEnabled(bool enabled) noexcept
{
    enabled_.setTarget(enabled ? 1.0f : 0.0f);
}

void Reverb::setHardBypass(bool bypass) noexcept
{
    hard_.setTarget(!bypass); // a = dry, b = processed
}

void Reverb::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (sampleRate_ <= 0.0) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    startPendingModeFade();
    const int chunk = static_cast<int>(scratch_[0].size());
    for (int pos = 0; pos < numSamples; pos += chunk)
        processChunk(inL + pos, inR + pos, outL + pos, outR + pos, std::min(chunk, numSamples - pos));
}

void Reverb::processChunk(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    float *tL = scratch_[0].data(), *tR = scratch_[1].data(), *wL = scratch_[2].data(), *wR = scratch_[3].data();
    float *pL = scratch_[4].data(), *pR = scratch_[5].data(), *en = scratch_[6].data();
    const int preLen = static_cast<int>(pre_[0].size());

    // Pre-delay and input filters (the tank input).
    for (int i = 0; i < n; ++i) {
        const float e = en[i] = enabled_.next();
        pre_[0][static_cast<std::size_t>(preWrite_)] = inL[i] * e;
        pre_[1][static_cast<std::size_t>(preWrite_)] = inR[i] * e;
        const float d = preDelay_.next();
        const int di = static_cast<int>(d);
        const float f = d - static_cast<float>(di);
        const float lc = lowCut_.next(), hc = highCut_.next();
        if (lc != cachedLow_) {
            cachedLow_ = lc;
            gHp_ = tptG(lc, sampleRate_);
        }
        if (hc != cachedHigh_) {
            cachedHigh_ = hc;
            gLp_ = tptG(hc, sampleRate_);
        }
        float* const dst[2] = {tL, tR};
        for (int c = 0; c < 2; ++c) {
            int r0 = preWrite_ - di, r1 = r0 - 1;
            r0 += r0 < 0 ? preLen : 0;
            r1 += r1 < 0 ? preLen : 0;
            const auto& buf = pre_[static_cast<std::size_t>(c)];
            const float x = buf[static_cast<std::size_t>(r0)] +
                            f * (buf[static_cast<std::size_t>(r1)] - buf[static_cast<std::size_t>(r0)]);
            // TPT one-pole HP then LP (Zavalishin §3.10): v = (x − s)·G, lp = v + s, s = lp + v, hp = x − lp.
            const float vh = (x - hp_.s[c]) * gHp_;
            const float lpOfX = vh + hp_.s[c];
            hp_.s[c] = lpOfX + vh;
            const float hp = x - lpOfX;
            const float vl = (hp - lp_.s[c]) * gLp_;
            const float y = vl + lp_.s[c];
            lp_.s[c] = y + vl;
            dst[c][i] = y;
        }
        if (++preWrite_ == preLen)
            preWrite_ = 0;
    }

    // Tank(s): the active engine, plus the previous one while a mode crossfade runs.
    processEngine(active_, tL, tR, wL, wR, n);
    if (modeFade_.isRamping()) {
        processEngine(previous_, tL, tR, pL, pR, n);
        for (int i = 0; i < n; ++i) {
            const float g = modeFade_.next();
            wL[i] = (1.0f - g) * pL[i] + g * wL[i]; // exactly the new engine at g = 1
            wR[i] = (1.0f - g) * pR[i] + g * wR[i];
        }
    }

    // Mix (and soft-bypass trails), then the hard-bypass crossfade against the dry input.
    for (int i = 0; i < n; ++i) {
        const float m = mix_.next();
        const float e = en[i];
        pL[i] = inL[i] * (1.0f - m * e) + wL[i] * m;
        pR[i] = inR[i] * (1.0f - m * e) + wR[i] * m;
    }
    Crossfader hardR = hard_; // same fade position for the right channel
    hard_.process(inL, pL, outL, n);
    hardR.process(inR, pR, outR, n);
}

} // namespace osp::dsp
