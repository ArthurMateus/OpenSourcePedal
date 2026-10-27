#include "osp/dsp/Modulation.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

float Modulation::syncBeats(std::string_view value) noexcept
{
    float scale = 1.0f;
    if (!value.empty() && (value.back() == 'D' || value.back() == 'T')) {
        scale = value.back() == 'D' ? 1.5f : 2.0f / 3.0f;
        value.remove_suffix(1);
    }
    constexpr std::array<std::string_view, 5> kNames{"1/1", "1/2", "1/4", "1/8", "1/16"};
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (value == kNames[i])
            return 4.0f / static_cast<float>(1 << i) * scale;
    return 0.0f;
}

void Modulation::Slot::setType(Type t) noexcept
{
    type = t;
    if (t == Type::chorus80s || t == Type::chorusWide || t == Type::flanger)
        chorus.setVoicing(t == Type::chorus80s    ? ChorusFlanger::Voicing::chorus80s
                          : t == Type::chorusWide ? ChorusFlanger::Voicing::chorusWide
                                                  : ChorusFlanger::Voicing::flanger);
    else if (t == Type::phaser || t == Type::vibe)
        phaser.setVoicing(t == Type::phaser ? Phaser::Voicing::phaser : Phaser::Voicing::vibe);
}

void Modulation::Slot::process(float* l, float* r, int n) noexcept
{
    switch (type) {
    case Type::phaser:
    case Type::vibe:
        phaser.process(l, r, n);
        break;
    case Type::tremolo:
        tremolo.process(l, r, n);
        break;
    default:
        chorus.process(l, r, n);
        break;
    }
}

template <typename F>
void Modulation::forEachEngine(F&& f) noexcept
{
    for (auto& s : slots_) {
        f(s.chorus);
        f(s.phaser);
        f(s.tremolo);
    }
}

Modulation::Modulation() noexcept
{
    for (auto& s : slots_)
        s.setType(Type::chorusWide);
}

void Modulation::prepare(double sampleRate, int maxBlockSize)
{
    maxBlock_ = std::max(1, maxBlockSize);
    forEachEngine([&](auto& e) { e.prepare(sampleRate, maxBlock_); });
    fadeL_.prepare(sampleRate, kTypeFadeSeconds);
    fadeR_.prepare(sampleRate, kTypeFadeSeconds);
    bufL_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
    bufR_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
    monoR_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
    reset();
}

void Modulation::reset() noexcept
{
    if (pending_)
        slots_[static_cast<std::size_t>(current())].setType(pendingType_);
    pending_ = false;
    forEachEngine([](auto& e) { e.reset(); });
    fadeL_.reset(fadeL_.target());
    fadeR_.reset(fadeR_.target());
}

void Modulation::startFade(Type type) noexcept
{
    const int idle = 1 - current();
    Slot& s = slots_[static_cast<std::size_t>(idle)];
    s.setType(type);
    s.chorus.reset();
    s.phaser.reset();
    s.tremolo.reset();
    fadeL_.setTarget(idle == 1);
    fadeR_.setTarget(idle == 1);
}

void Modulation::setType(Type type) noexcept
{
    if (fadeL_.isFading()) {
        pending_ = true;
        pendingType_ = type;
        return;
    }
    pending_ = false;
    if (type != slots_[static_cast<std::size_t>(current())].type)
        startFade(type); // before prepare() the crossfader jumps, so the new type applies at once
}

float Modulation::effectiveRateHz() const noexcept
{
    const float hz = syncBeats_ > 0.0f && bpm_ > 0.0f ? bpm_ / 60.0f / syncBeats_ : rateHz_;
    return std::isnan(hz) ? ChorusFlanger::kMinRateHz
                          : std::clamp(hz, ChorusFlanger::kMinRateHz, ChorusFlanger::kMaxRateHz);
}

void Modulation::applyRate() noexcept
{
    const float hz = effectiveRateHz();
    forEachEngine([&](auto& e) { e.setRateHz(hz); });
}

void Modulation::setRateHz(float hz) noexcept
{
    rateHz_ = hz;
    applyRate();
}
void Modulation::setSyncBeats(float beats) noexcept
{
    syncBeats_ = std::isfinite(beats) && beats > 0.0f ? beats : 0.0f;
    applyRate();
}
void Modulation::setTempoBpm(float bpm) noexcept
{
    bpm_ = std::isfinite(bpm) && bpm > 0.0f ? bpm : 0.0f;
    applyRate();
}
void Modulation::setDepthPct(float pct) noexcept
{
    forEachEngine([&](auto& e) { e.setDepthPct(pct); });
}
void Modulation::setMixPct(float pct) noexcept
{
    forEachEngine([&](auto& e) { e.setMixPct(pct); });
}
void Modulation::setFeedbackPct(float pct) noexcept
{
    for (auto& s : slots_) {
        s.chorus.setFeedbackPct(pct);
        s.phaser.setFeedbackPct(pct);
    }
}
void Modulation::setWidthPct(float pct) noexcept
{
    forEachEngine([&](auto& e) { e.setWidthPct(pct); });
}
void Modulation::setEnabled(bool on) noexcept
{
    forEachEngine([&](auto& e) { e.setEnabled(on); });
}

void Modulation::processChunk(float* left, float* right, int n) noexcept
{
    if (pending_ && !fadeL_.isFading()) {
        pending_ = false;
        if (pendingType_ != slots_[static_cast<std::size_t>(current())].type)
            startFade(pendingType_);
    }
    if (!fadeL_.isFading()) {
        slots_[static_cast<std::size_t>(current())].process(left, right, n);
        return;
    }
    std::copy(left, left + n, bufL_.begin());
    std::copy(right, right + n, bufR_.begin());
    slots_[0].process(left, right, n);
    slots_[1].process(bufL_.data(), bufR_.data(), n);
    fadeL_.process(left, bufL_.data(), left, n); // a = slot 0, b = slot 1
    fadeR_.process(right, bufR_.data(), right, n);
}

void Modulation::processStereo(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += maxBlock_)
        processChunk(left + pos, right + pos, std::min(maxBlock_, numSamples - pos));
}

void Modulation::processMono(float* samples, int numSamples) noexcept
{
    if (numSamples <= 0 || samples == nullptr || bypassed_)
        return;
    if (!isPrepared()) {
        std::fill(samples, samples + numSamples, 0.0f);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += maxBlock_) {
        const int n = std::min(maxBlock_, numSamples - pos);
        float* x = samples + pos;
        std::copy(x, x + n, monoR_.begin());
        processChunk(x, monoR_.data(), n);
        for (int i = 0; i < n; ++i)
            x[i] = 0.5f * (x[i] + monoR_[static_cast<std::size_t>(i)]); // disabled: 0.5·(x + x) == x exactly
    }
}

} // namespace osp::dsp
