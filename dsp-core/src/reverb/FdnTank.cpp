#include "osp/dsp/reverb/FdnTank.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>

namespace osp::dsp {
namespace {

constexpr double kDampHz = 10000.0;  // in-loop one-pole damping corner
constexpr double kMidHz = 707.1;     // RT60 reference: geometric centre of 500 Hz…1 kHz
constexpr double kMaxLoopGain = 0.99999; // the compensated gain must stay below lossless outside freeze

bool isPrime(int n)
{
    if (n < 2)
        return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return false;
    return true;
}

int nextPrime(int n)
{
    while (!isPrime(n))
        ++n;
    return n;
}

// In-place 8-point fast Walsh-Hadamard transform, scaled to be orthonormal.
void hadamard8(std::array<float, FdnTank::kChannels>& v) noexcept
{
    for (int h = 1; h < 8; h *= 2)
        for (int i = 0; i < 8; i += 2 * h)
            for (int j = i; j < i + h; ++j) {
                const float a = v[static_cast<std::size_t>(j)], b = v[static_cast<std::size_t>(j + h)];
                v[static_cast<std::size_t>(j)] = a + b;
                v[static_cast<std::size_t>(j + h)] = a - b;
            }
    for (auto& x : v)
        x *= 0.35355339f; // 1/√8
}

} // namespace

float FdnTank::Line::at(int delay) const noexcept
{
    int r = write - delay;
    if (r < 0)
        r += static_cast<int>(buf.size());
    return buf[static_cast<std::size_t>(r)];
}

float FdnTank::Line::frac(float delay) const noexcept
{
    const int i = static_cast<int>(delay);
    const float f = delay - static_cast<float>(i);
    const float a = at(i), b = at(i + 1);
    return a + f * (b - a);
}

void FdnTank::prepare(double sampleRate, const Config& c)
{
    sampleRate_ = sampleRate;
    const double msToSamples = sampleRate / 1000.0;
    modDepth_ = static_cast<float>(c.modDepthSamples48k * sampleRate / 48000.0);
    lfoInc_ = 2.0 * std::numbers::pi * c.lfoHz / sampleRate;

    // Diffuser: per stage, channel k gets a fixed pseudo-random delay in (0, stage length] (deterministic LCG).
    diffuser_.assign(static_cast<std::size_t>(c.diffuserStages), {});
    diffuserDelay_.assign(static_cast<std::size_t>(c.diffuserStages), {});
    const double stageSamples = c.diffuserMs * msToSamples / std::max(c.diffuserStages, 1);
    std::uint32_t seed = 0x5eed1234u;
    for (std::size_t s = 0; s < diffuser_.size(); ++s)
        for (std::size_t k = 0; k < kChannels; ++k) {
            seed = seed * 1664525u + 1013904223u;
            const double r = (static_cast<double>(k) + static_cast<double>(seed >> 8) / 16777216.0) / kChannels;
            const int d = std::max(1, static_cast<int>(r * stageSamples));
            diffuserDelay_[s][k] = d;
            diffuser_[s][k].buf.assign(static_cast<std::size_t>(d + 1), 0.0f);
        }

    // FDN lines: geometric spacing, rounded up to distinct primes (mutually coprime lengths avoid coinciding modes).
    const int maxMod = static_cast<int>(std::ceil(modDepth_)) + 2;
    for (std::size_t i = 0; i < kChannels; ++i) {
        const double ms = c.lineMinMs * std::pow(c.lineMaxMs / c.lineMinMs, static_cast<double>(i) / (kChannels - 1));
        int len = nextPrime(static_cast<int>(ms * msToSamples));
        while (i > 0 && len <= length_[i - 1])
            len = nextPrime(len + 1);
        length_[i] = len;
        lines_[i].buf.assign(static_cast<std::size_t>(len + maxMod + 2), 0.0f);
    }

    dampCoeff_ = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * kDampHz / sampleRate));
    // The decay parameter is the mid-band RT60 (ISO 3382-1 T_mid: the mean of the 500 Hz and 1 kHz octave T30s), so the
    // damping loss at their geometric centre is compensated in the loop gains. Above it, the tail decays faster.
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * kMidHz / sampleRate);
    dampCompensation_ = std::abs(1.0 - (1.0 - dampCoeff_) * z) / dampCoeff_;
    decay_.prepare(sampleRate, 0.020);
    mod_.prepare(sampleRate, 0.020);
    freeze_.prepare(sampleRate, 0.050);
    reset();
}

void FdnTank::reset() noexcept
{
    for (auto& stage : diffuser_)
        for (auto& l : stage) {
            std::fill(l.buf.begin(), l.buf.end(), 0.0f);
            l.write = 0;
        }
    for (auto& l : lines_) {
        std::fill(l.buf.begin(), l.buf.end(), 0.0f);
        l.write = 0;
    }
    lp_.fill(0.0f);
    for (std::size_t i = 0; i < kChannels; ++i)
        lfoPhase_[i] = 2.0 * std::numbers::pi * static_cast<double>(i) / kChannels; // staggered phases
    decay_.reset(decay_.target());
    mod_.reset(mod_.target());
    freeze_.reset(freeze_.target());
    cachedDecay_ = -1.0f;
    updateGains();
}

void FdnTank::updateGains() noexcept
{
    const float rt60 = std::clamp(decay_.current(), 0.3f, 20.0f);
    if (rt60 == cachedDecay_)
        return;
    cachedDecay_ = rt60;
    for (std::size_t i = 0; i < kChannels; ++i)
        gain_[i] = static_cast<float>(
            std::min(std::pow(10.0, -3.0 * length_[i] / (sampleRate_ * rt60)) * dampCompensation_, kMaxLoopGain));
}

void FdnTank::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    if (sampleRate_ <= 0.0) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    std::array<float, kChannels> v{}, r{};
    for (int n = 0; n < numSamples; ++n) {
        decay_.next();
        updateGains(); // recomputes only while the decay smoother moves (parameters.md §1.4: per sample)
        const float depth = modDepth_ * mod_.next();
        const float frz = freeze_.next();
        const float inGain = 1.0f - frz;

        // Input: L feeds channels 0–3, R feeds 4–7, then the diffuser stages.
        for (std::size_t k = 0; k < kChannels; ++k)
            v[k] = inGain * (k < 4 ? inL[n] : inR[n]);
        for (std::size_t s = 0; s < diffuser_.size(); ++s) {
            for (std::size_t k = 0; k < kChannels; ++k) {
                auto& l = diffuser_[s][k];
                l.push(v[k]);
                v[k] = l.at(diffuserDelay_[s][k]);
            }
            hadamard8(v);
            for (std::size_t k = (s % 2); k < kChannels; k += 2)
                v[k] = -v[k]; // polarity flips alternate per stage
        }

        // Tank: modulated reads, damping (bypassed while frozen), decay gains (→ 1 while frozen).
        float sum = 0.0f;
        for (std::size_t i = 0; i < kChannels; ++i) {
            const float m = depth * (1.0f - frz) * static_cast<float>(std::sin(lfoPhase_[i]));
            lfoPhase_[i] += lfoInc_;
            if (lfoPhase_[i] > 2.0 * std::numbers::pi)
                lfoPhase_[i] -= 2.0 * std::numbers::pi;
            const float x = lines_[i].frac(static_cast<float>(length_[i]) + m);
            lp_[i] += dampCoeff_ * (x - lp_[i]);
            const float damped = lp_[i] + frz * (x - lp_[i]);
            r[i] = damped * (gain_[i] + frz * (1.0f - gain_[i]));
            sum += r[i];
        }
        const float h = sum * (2.0f / kChannels);
        float l = 0.0f, rr = 0.0f;
        for (std::size_t i = 0; i < kChannels; ++i) {
            lines_[i].push(r[i] - h + v[i]);
            const float sgn = (i / 2) % 2 ? -1.0f : 1.0f;
            (i % 2 ? rr : l) += sgn * r[i];
        }
        outL[n] = 0.5f * l;
        outR[n] = 0.5f * rr;
    }
}

} // namespace osp::dsp
