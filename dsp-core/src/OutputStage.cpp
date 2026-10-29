#include "osp/dsp/OutputStage.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

OutputStage::OutputStage() noexcept
{
    ceiling_ = std::pow(10.0f, (kCeilingDb - kTpMarginDb) / 20.0f);
    // 4x interpolation between hist[c - 1] and hist[c], c = kTpTaps / 2: tap k sits at offset k - (c - 1) samples
    // from the left point, so phase p/4 weighs it by sinc(p/4 - (k - c + 1)), Hann-windowed over the span, and each
    // phase is normalised to unity DC gain.
    constexpr int c = kTpTaps / 2;
    constexpr double pi = 3.14159265358979323846, half = kTpTaps / 2.0 + 0.5;
    for (int p = 1; p <= 3; ++p) {
        double sum = 0.0;
        std::array<double, kTpTaps> h{};
        for (int k = 0; k < kTpTaps; ++k) {
            const double t = p / 4.0 - static_cast<double>(k - c + 1);
            const double sinc = t == 0.0 ? 1.0 : std::sin(pi * t) / (pi * t);
            h[static_cast<std::size_t>(k)] = sinc * (0.5 + 0.5 * std::cos(pi * t / half));
            sum += h[static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < kTpTaps; ++k)
            tp_[static_cast<std::size_t>(k)][static_cast<std::size_t>(p - 1)] =
                static_cast<float>(h[static_cast<std::size_t>(k)] / sum);
    }
}

void OutputStage::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    holdSamples_ = std::max(1, static_cast<int>(std::lround(kHoldSeconds * sampleRate)));
    level_.prepare(sampleRate, 0.020);
    limiterMix_.prepare(sampleRate, kSwitchSeconds);
    monoMix_.prepare(sampleRate, kSwitchSeconds);
    release_ = static_cast<float>(std::exp(1.0 / (kReleaseSeconds * sampleRate)));
    reset();
}

void OutputStage::reset() noexcept
{
    level_.reset(level_.target());
    limiterMix_.reset(limiterMix_.target());
    monoMix_.reset(monoMix_.target());
    gain_ = 1.0f;
    holdRemaining_ = 0;
    hist_ = {};
    peak_ = {};
    rms_ = {};
}

void OutputStage::setLevelDb(float dB) noexcept
{
    const float d = std::isnan(dB) ? kMinLevelDb : std::clamp(dB, kMinLevelDb, kMaxLevelDb);
    level_.setTarget(std::pow(10.0f, d / 20.0f));
}

float OutputStage::gainReductionDb() const noexcept
{
    return gain_ < 1.0f ? -20.0f * std::log10(gain_) : 0.0f;
}

void OutputStage::process(float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0 || left == nullptr || right == nullptr)
        return;
    if (!isPrepared()) {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    double sum[2] = {0.0, 0.0};
    float pk[2] = {0.0f, 0.0f};
    for (int i = 0; i < numSamples; ++i) {
        float l = left[i], r = right[i];
        if (!std::isfinite(l)) { // RT-9
            l = 0.0f;
            nan_ = true;
        }
        if (!std::isfinite(r)) {
            r = 0.0f;
            nan_ = true;
        }
        if (!bypassed_) {
            const float g = level_.next();
            l *= g;
            r *= g;
            const float m = monoMix_.next();
            if (m > 0.0f) {
                const float s = 0.5f * (l + r);
                if (m == 1.0f) {
                    l = r = s; // settled: exactly equal channels
                } else {
                    l += m * (s - l);
                    r += m * (s - r);
                }
            }
            // Detector: always runs, so switching the limiter on finds a warm state.
            float e = std::max(std::abs(l), std::abs(r));
            const float in[2] = {l, r};
            for (std::size_t c = 0; c < 2; ++c) {
                auto& h = hist_[c];
                std::copy(h.begin() + 1, h.end(), h.begin());
                h.back() = in[c];
                std::array<float, 4> values{};
                // Independent phase accumulators retain each sum's order and let the compiler pack four lanes.
                // The fourth coefficient lane is zero; only the three fractional phases enter the detector.
                for (std::size_t k = 0; k < h.size(); ++k)
                    for (std::size_t p = 0; p < values.size(); ++p)
                        values[p] += tp_[k][p] * h[k];
                for (std::size_t p = 0; p < 3; ++p)
                    e = std::max(e, std::abs(values[p]));
            }
            const float target = ceiling_ / std::max(e, ceiling_);
            holdRemaining_ = target < 1.0f && target <= gain_ ? holdSamples_ : std::max(0, holdRemaining_ - 1);
            const float release = holdRemaining_ > 0 ? 1.0f : release_;
            // A floor lets a gain flushed to zero by FTZ recover; normal gain reaches exactly unity.
            const float released = std::min(1.0f, std::max(gain_, 1.0e-12f) * release);
            gain_ = std::min(target, released);
            const float mix = limiterMix_.next();
            l += mix * (l * gain_ - l); // gain_ == 1: l + mix * 0 == l exactly
            r += mix * (r * gain_ - r);
        }
        left[i] = l;
        right[i] = r;
        pk[0] = std::max(pk[0], std::abs(l));
        pk[1] = std::max(pk[1], std::abs(r));
        sum[0] += static_cast<double>(l) * l;
        sum[1] += static_cast<double>(r) * r;
    }
    for (int c = 0; c < 2; ++c) {
        peak_[static_cast<std::size_t>(c)] = pk[c];
        rms_[static_cast<std::size_t>(c)] = static_cast<float>(std::sqrt(sum[c] / numSamples));
    }
}

} // namespace osp::dsp
