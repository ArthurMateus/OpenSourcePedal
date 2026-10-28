#include "osp/dsp/reverb/ShimmerTank.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace osp::dsp {
namespace {

constexpr double kGrainSeconds = 0.040;
constexpr double kDetuneCents = 2.0; // L below, R above the interval
constexpr double kLowPassHz = 4500.0, kHighPassHz = 100.0;
constexpr std::array<double, 2> kDiffuserMs{7.0, 11.0};
constexpr float kDiffusion = 0.65f;
constexpr float kReturnGain = 0.6f; // at 100 % shimmer and decay <= kReturnDecay, before tanh
constexpr float kReturnDecay = 2.5f;

// Snaps the loop's decaying states at −400 dB: a near-lossless recursion can limit-cycle just above the smallest
// normal float, where FTZ does not reach (measured on the spring tank, #73).
float snap(float v) noexcept
{
    return std::abs(v) < 1.0e-20f ? 0.0f : v;
}

} // namespace

double ShimmerTank::ratio(Interval interval) noexcept
{
    switch (interval) {
    case Interval::FifthUp:
        return std::exp2(7.0 / 12.0);
    case Interval::OctDown:
        return 0.5;
    case Interval::OctUp:
    default:
        return 2.0;
    }
}

float ShimmerTank::Allpass::process(float x, float g) noexcept
{
    const float d = buf[static_cast<std::size_t>(write)];
    const float w = x + g * d;
    buf[static_cast<std::size_t>(write)] = w;
    if (++write == static_cast<int>(buf.size()))
        write = 0;
    return d - g * w;
}

void ShimmerTank::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    tank_.prepare(sampleRate, FdnTank::kHall);
    for (Path& p : path_) {
        p.shifter.prepare(sampleRate, kGrainSeconds);
        for (std::size_t i = 0; i < p.diffuser.size(); ++i)
            p.diffuser[i].buf.assign(
                static_cast<std::size_t>(std::max(1L, std::lround(kDiffuserMs[i] * 1e-3 * sampleRate))), 0.0f);
    }
    lp_ = svfDesign(SvfType::lowPass, sampleRate, kLowPassHz, 0.70710678, 0.0);
    hp_ = svfDesign(SvfType::highPass, sampleRate, kHighPassHz, 0.70710678, 0.0);
    return_.prepare(sampleRate, 0.020);
    intervalGain_.prepare(sampleRate, 0.025);
    reset();
}

void ShimmerTank::reset() noexcept
{
    tank_.reset();
    for (Path& p : path_) {
        p.shifter.reset();
        p.preLp = p.lp = p.hp = {};
        for (Allpass& a : p.diffuser) {
            std::fill(a.buf.begin(), a.buf.end(), 0.0f);
            a.write = 0;
        }
        p.ring.fill(0.0f);
    }
    ringPos_ = 0;
    return_.reset(return_.target());
    intervalGain_.reset(1.0f);
    applyInterval();
}

void ShimmerTank::updateReturn() noexcept
{
    const float scale = decay_ > kReturnDecay ? std::sqrt(kReturnDecay / decay_) : 1.0f;
    return_.setTarget(kReturnGain * amount_ * scale);
}

void ShimmerTank::applyInterval() noexcept
{
    current_ = pending_;
    const double r = ratio(current_);
    path_[0].shifter.setRatio(r * std::exp2(-kDetuneCents / 1200.0));
    path_[1].shifter.setRatio(r * std::exp2(kDetuneCents / 1200.0));
}

void ShimmerTank::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    if (sampleRate_ <= 0.0) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    for (int pos = 0; pos < numSamples; pos += kLoopChunk)
        processChunk(inL + pos, inR + pos, outL + pos, outR + pos, std::min(kLoopChunk, numSamples - pos));
}

void ShimmerTank::processChunk(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    // Every ring slot read here was written 32 samples ago, and is rewritten below after its read.
    const float* const in[2] = {inL, inR};
    for (std::size_t c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            in_[c][static_cast<std::size_t>(i)] =
                in[c][i] + path_[c].ring[static_cast<std::size_t>((ringPos_ + i) % kLoopChunk)];
    tank_.process(in_[0].data(), in_[1].data(), outL, outR, n);

    float* const out[2] = {outL, outR};
    for (int i = 0; i < n; ++i) {
        if (pending_ != current_ && !intervalGain_.isRamping()) {
            if (intervalGain_.current() <= 0.0f) {
                applyInterval();
                intervalGain_.setTarget(1.0f);
            } else {
                intervalGain_.setTarget(0.0f);
            }
        }
        const float gain = return_.next() * intervalGain_.next();
        const auto slot = static_cast<std::size_t>((ringPos_ + i) % kLoopChunk);
        for (std::size_t c = 0; c < 2; ++c) {
            Path& p = path_[c];
            float s = p.shifter.process(p.preLp.tick(lp_, out[c][i]));
            s = p.hp.tick(hp_, p.lp.tick(lp_, s));
            s = p.diffuser[1].process(p.diffuser[0].process(s, kDiffusion), kDiffusion);
            p.ring[slot] = snap(std::tanh(gain * s));
        }
    }
    ringPos_ = (ringPos_ + n) % kLoopChunk;
}

} // namespace osp::dsp
