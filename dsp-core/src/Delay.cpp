#include "osp/dsp/Delay.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
float finiteOrZero(float v) noexcept
{
    return std::isfinite(v) ? v : 0.0f;
}
} // namespace

Delay::Delay() noexcept
{
    duckDb_.reset(0.0f);
}

void Delay::prepare(double sampleRate, int maxBlockSize)
{
    maxBlock_ = std::max(maxBlockSize, 1);
    forEachSlot([&](DelayVoice& v) { v.prepare(sampleRate, maxBlock_); });
    const auto size = static_cast<std::size_t>(maxBlock_);
    sendL_.assign(size, 0.0f);
    sendR_.assign(size, 0.0f);
    for (int i = 0; i < 2; ++i) {
        wetL_[i].assign(size, 0.0f);
        wetR_[i].assign(size, 0.0f);
    }
    fade_.prepare(sampleRate, kTypeFadeSeconds);
    mix_.prepare(sampleRate, 0.020);
    enabled_.prepare(sampleRate, 0.010);
    send_.prepare(sampleRate, 0.010);
    duckDb_.prepare(sampleRate, 0.020);
    attack_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate)));
    release_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.200 * sampleRate)));
    reset();
}

void Delay::reset() noexcept
{
    fade_.reset(static_cast<float>(activeSlot()));
    forEachSlot([&](DelayVoice& v) {
        v.setVoicing(mode_);
        v.setStereo(stereo_);
        v.reset();
    });
    mix_.reset(mix_.target());
    enabled_.reset(enabled_.target());
    send_.reset(send_.target());
    duckDb_.reset(duckDb_.target());
    env_ = 0.0f;
    voicesIdle_ = enabled_.target() == 0.0f;
}

void Delay::setMode(Mode m) noexcept
{
    mode_ = m;
    if (!isPrepared())
        forEachSlot([&](DelayVoice& v) { v.setVoicing(m); });
}
void Delay::setStereo(Stereo s) noexcept
{
    stereo_ = s;
    if (!isPrepared())
        forEachSlot([&](DelayVoice& v) { v.setStereo(s); });
}
void Delay::setTimeMs(float ms) noexcept
{
    timeMs_ = std::isnan(ms) ? DelayVoice::kMinMs : std::clamp(ms, DelayVoice::kMinMs, DelayVoice::kMaxMs);
    updateTime();
}
void Delay::setSync(SyncDivision d) noexcept
{
    const int i = static_cast<int>(d);
    sync_ = i > 0 && i < TempoSync::kNumDivisions ? d : SyncDivision::off;
    updateTime();
}
void Delay::setTempoBpm(double bpm) noexcept
{
    bpm_ = bpm;
    updateTime();
}
void Delay::updateTime() noexcept
{
    const double synced = TempoSync::timeMs(sync_, bpm_);
    effectiveMs_ = synced > 0.0 ? static_cast<float>(synced) : timeMs_;
    forEachSlot([&](DelayVoice& v) { v.setTimeMs(effectiveMs_); });
}
void Delay::setFeedbackPct(float pct) noexcept
{
    forEachSlot([&](DelayVoice& v) { v.setFeedbackPct(pct); });
}
void Delay::setMixPct(float pct) noexcept
{
    mix_.setTarget(std::isnan(pct) ? 0.0f : std::clamp(pct, 0.0f, 100.0f) / 100.0f);
}
void Delay::setLowCutHz(float hz) noexcept
{
    forEachSlot([&](DelayVoice& v) { v.setLowCutHz(hz); });
}
void Delay::setHighCutHz(float hz) noexcept
{
    forEachSlot([&](DelayVoice& v) { v.setHighCutHz(hz); });
}
void Delay::setModDepthPct(float pct) noexcept
{
    forEachSlot([&](DelayVoice& v) { v.setModDepthPct(pct); });
}
void Delay::setSaturationPct(float pct) noexcept
{
    forEachSlot([&](DelayVoice& v) { v.setSaturationPct(pct); });
}
void Delay::setDuckDb(float db) noexcept
{
    duckDb_.setTarget(std::isnan(db) ? 0.0f : std::clamp(db, 0.0f, kMaxDuckDb));
}

void Delay::startSwitchIfNeeded() noexcept
{
    const int a = activeSlot();
    if (fade_.isRamping() || (slots_[a].voicing() == mode_ && slots_[a].stereo() == stereo_))
        return;
    DelayVoice& next = slots_[1 - a];
    next.copyStateFrom(slots_[a]);
    next.setVoicing(mode_);
    next.setStereo(stereo_);
    fade_.setTarget(static_cast<float>(1 - a));
}

void Delay::processChunk(float* left, float* right, int n) noexcept
{
    if (voicesIdle_ && enabled_.target() > 0.0f) { // re-enabled: start from silence, not from a stale tail
        forEachSlot([](DelayVoice& v) { v.reset(); });
        env_ = 0.0f;
        voicesIdle_ = false;
    }
    if (voicesIdle_) {
        for (int k = 0; k < n; ++k) {
            left[k] = finiteOrZero(left[k]);
            right[k] = finiteOrZero(right[k]);
        }
        return;
    }
    startSwitchIfNeeded();
    for (int k = 0; k < n; ++k) {
        const float s = send_.next();
        sendL_[static_cast<std::size_t>(k)] = s * finiteOrZero(left[k]);
        sendR_[static_cast<std::size_t>(k)] = s * finiteOrZero(right[k]);
    }
    const bool fading = fade_.isRamping();
    const int a = activeSlot();
    for (int i = 0; i < 2; ++i)
        if (fading || i == a)
            slots_[i].processWet(sendL_.data(), sendR_.data(), wetL_[i].data(), wetR_[i].data(), n);
    for (int k = 0; k < n; ++k) {
        const auto j = static_cast<std::size_t>(k);
        float wl = wetL_[a][j], wr = wetR_[a][j];
        if (fade_.isRamping()) {
            const float x = fade_.next(); // linear: both slots are time-aligned and correlated (signal-flow §5)
            wl = (1.0f - x) * wetL_[0][j] + x * wetL_[1][j];
            wr = (1.0f - x) * wetR_[0][j] + x * wetR_[1][j];
        }
        const float l = finiteOrZero(left[k]), r = finiteOrZero(right[k]);
        const float peak = std::max(std::abs(l), std::abs(r));
        env_ += (peak > env_ ? attack_ : release_) * (peak - env_);
        if (env_ < 1.0e-20f)
            env_ = 0.0f;
        const float depth = duckDb_.next();
        float duck = 1.0f;
        if (depth > 0.0f) {
            const float envDb = 20.0f * std::log10(env_ + 1.0e-12f);
            const float amount = std::clamp((envDb - kDuckFloorDb) / kDuckKneeDb, 0.0f, 1.0f);
            duck = std::pow(10.0f, -0.05f * depth * amount);
        }
        const float g = mix_.next() * enabled_.next() * duck;
        left[k] = l + g * wl; // g == 0: exactly the dry signal
        right[k] = r + g * wr;
    }
    if (enabled_.target() == 0.0f && !enabled_.isRamping())
        voicesIdle_ = true;
}

void Delay::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += maxBlock_) {
        const int n = std::min(maxBlock_, numSamples - pos);
        processChunk(left + pos, right + pos, n);
    }
}

} // namespace osp::dsp
