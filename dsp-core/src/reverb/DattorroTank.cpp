#include "osp/dsp/reverb/DattorroTank.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace osp::dsp {
namespace {

constexpr double kPaperRate = 29761.0;
// Dattorro 1997, Table 1 (lengths at 29761 Hz) and the coefficients of Fig. 1 (see the header for provenance).
constexpr std::array<int, 4> kDiffuserLengths{142, 107, 379, 277};
constexpr float kInputDiffusion1 = 0.75f, kInputDiffusion2 = 0.625f;
constexpr float kDecayDiffusion1 = 0.70f;
// Decay diffusion 2 tracks the decay gain: g + 0.15, clamped to [0.25, 0.50]. Its allpass recirculates inside the
// loop, beyond the decay gains, so a fixed 0.5 would floor the RT60 near 0.9 s (measured at 48 kHz).
constexpr float kDecayDiffusion2Min = 0.25f, kDecayDiffusion2Max = 0.50f, kDecayDiffusion2Offset = 0.15f;
constexpr float kBandwidth = 0.9995f, kDamping = 0.0005f;
constexpr int kModL = 672, kDelayL1 = 4453, kApL = 1800, kDelayL2 = 3720;
constexpr int kModR = 908, kDelayR1 = 4217, kApR = 2656, kDelayR2 = 3163;
constexpr double kExcursion = 12.0; // samples at 29761 Hz, at 100 % modulation (CAPS 0.9.26)
constexpr double kLfoHz = 1.0;
constexpr float kOutputGain = 0.6f;
// Table 2 tap offsets at 29761 Hz, in the order they are summed in process().
constexpr std::array<int, 14> kTaps{266, 2974, 1913, 1996, 1990, 187, 1066, 353, 3627, 1228, 2673, 2111, 335, 121};
constexpr double kMidHz = 707.1; // RT60 reference, as in FdnTank
// Measured calibration: with the loop time as the plain sum of lengths, T_mid came out 5.7-8.6 % long over 1-15 s
// (mean 7.1 %, 48 kHz, PR description). Late modes sit where the in-loop allpasses' group delay peaks
// (N·(1+g)/(1−g)), so the energy circulates longer than Σ N. The gains use T_loop × 1.071 instead.
constexpr double kLoopTimeCalibration = 1.071;
constexpr double kMaxLoopGain = 0.99999; // below lossless outside freeze

int scaled(int samples, double scale) noexcept
{
    return std::max(1, static_cast<int>(std::lround(samples * scale)));
}

} // namespace

float DattorroTank::Line::at(int delay) const noexcept
{
    int r = write - delay;
    if (r < 0)
        r += static_cast<int>(buf.size());
    return buf[static_cast<std::size_t>(r)];
}

float DattorroTank::Line::frac(float delay) const noexcept
{
    const int i = static_cast<int>(delay);
    const float f = delay - static_cast<float>(i);
    const float a = at(i), b = at(i + 1);
    return a + f * (b - a);
}

float DattorroTank::Allpass::process(float x, float g) noexcept
{
    const float d = line.at(length);
    const float w = x + g * d;
    line.push(w);
    return d - g * w;
}

float DattorroTank::Allpass::processModulated(float x, float g, float delay) noexcept
{
    const float d = line.frac(delay);
    const float w = x + g * d;
    line.push(w);
    return d - g * w;
}

void DattorroTank::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    scale_ = sampleRate / kPaperRate;
    excursion_ = static_cast<float>(kExcursion * scale_);
    const int extra = static_cast<int>(std::ceil(excursion_)) + 2;
    auto makeAllpass = [&](Allpass& ap, int paperLength, int margin) {
        ap.length = scaled(paperLength, scale_);
        ap.line.buf.assign(static_cast<std::size_t>(ap.length + margin + 1), 0.0f);
    };
    for (std::size_t i = 0; i < diffuser_.size(); ++i)
        makeAllpass(diffuser_[i], kDiffuserLengths[i], 0);
    makeAllpass(modL_, kModL, extra);
    makeAllpass(modR_, kModR, extra);
    makeAllpass(apL_, kApL, 0);
    makeAllpass(apR_, kApR, 0);
    auto makeLine = [&](Line& l, int& len, int paperLength) {
        len = scaled(paperLength, scale_);
        l.buf.assign(static_cast<std::size_t>(len + 1), 0.0f);
    };
    makeLine(delayL1_, lenL1_, kDelayL1);
    makeLine(delayL2_, lenL2_, kDelayL2);
    makeLine(delayR1_, lenR1_, kDelayR1);
    makeLine(delayR2_, lenR2_, kDelayR2);
    for (std::size_t i = 0; i < taps_.size(); ++i)
        taps_[i] = scaled(kTaps[i], scale_);

    lfoInc_ = 2.0 * std::numbers::pi * kLfoHz / sampleRate;
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * kMidHz / sampleRate);
    dampCompensation_ = std::abs(1.0 - double(kDamping) * z) / (1.0 - kDamping);
    decay_.prepare(sampleRate, 0.020);
    mod_.prepare(sampleRate, 0.020);
    freeze_.prepare(sampleRate, 0.050);
    reset();
}

void DattorroTank::reset() noexcept
{
    auto clear = [](Line& l) {
        std::fill(l.buf.begin(), l.buf.end(), 0.0f);
        l.write = 0;
    };
    for (auto& ap : diffuser_)
        clear(ap.line);
    for (Allpass* ap : {&modL_, &modR_, &apL_, &apR_})
        clear(ap->line);
    for (Line* l : {&delayL1_, &delayL2_, &delayR1_, &delayR2_})
        clear(*l);
    lfoPhase_ = 0.0;
    bandwidthState_ = dampL_ = dampR_ = fbL_ = fbR_ = 0.0f;
    decay_.reset(decay_.target());
    mod_.reset(mod_.target());
    freeze_.reset(freeze_.target());
    cachedDecay_ = -1.0f;
    updateGains();
}

void DattorroTank::updateGains() noexcept
{
    const float rt60 = std::clamp(decay_.current(), 0.3f, 20.0f);
    if (rt60 == cachedDecay_ || sampleRate_ <= 0.0)
        return;
    cachedDecay_ = rt60;
    const double loopSeconds =
        double(modL_.length + lenL1_ + apL_.length + lenL2_ + modR_.length + lenR1_ + apR_.length + lenR2_) /
        sampleRate_;
    const double g = std::pow(10.0, -3.0 * loopSeconds * kLoopTimeCalibration / (4.0 * rt60)); // 4 gains per loop
    gain_ = static_cast<float>(std::min(g, kMaxLoopGain));
    gainDamped_ = static_cast<float>(std::min(g * dampCompensation_, kMaxLoopGain));
    decayDiffusion2_ =
        std::clamp(static_cast<float>(g) + kDecayDiffusion2Offset, kDecayDiffusion2Min, kDecayDiffusion2Max);
}

void DattorroTank::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    if (sampleRate_ <= 0.0) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    for (int n = 0; n < numSamples; ++n) {
        decay_.next();
        updateGains(); // recomputes only while the decay smoother moves (parameters.md §1.4: per sample)
        const float frz = freeze_.next();
        const float depth = excursion_ * mod_.next() * (1.0f - frz);
        const float g1 = gainDamped_ + frz * (1.0f - gainDamped_);
        const float g2 = gain_ + frz * (1.0f - gain_);

        // Input: mono sum, bandwidth one-pole, four diffusers.
        const float x = 0.5f * (inL[n] + inR[n]) * (1.0f - frz);
        bandwidthState_ = kBandwidth * x + (1.0f - kBandwidth) * bandwidthState_;
        float d = diffuser_[0].process(bandwidthState_, kInputDiffusion1);
        d = diffuser_[1].process(d, kInputDiffusion1);
        d = diffuser_[2].process(d, kInputDiffusion2);
        d = diffuser_[3].process(d, kInputDiffusion2);

        // Tank: each half takes the other half's output from the previous sample.
        const auto lfo = static_cast<float>(std::sin(lfoPhase_));
        const auto lfoQ = static_cast<float>(std::cos(lfoPhase_)); // the right half's LFO, 90° apart
        lfoPhase_ += lfoInc_;
        if (lfoPhase_ > 2.0 * std::numbers::pi)
            lfoPhase_ -= 2.0 * std::numbers::pi;
        const float fbL = fbL_, fbR = fbR_;
        auto half = [&](float in, Allpass& mod, float modDelay, Line& d1, int len1, float& damp, Allpass& ap, Line& d2,
                        int len2) {
            const float a = mod.processModulated(in, -kDecayDiffusion1, modDelay);
            const float b = d1.at(len1);
            d1.push(a);
            damp = (1.0f - kDamping) * b + kDamping * damp;
            const float damped = damp + frz * (b - damp); // damping off while frozen
            const float e = ap.process(damped * g1, decayDiffusion2_);
            const float f = d2.at(len2);
            d2.push(e);
            return f * g2;
        };
        fbL_ = half(d + fbR, modL_, static_cast<float>(modL_.length) + depth * lfo, delayL1_, lenL1_, dampL_, apL_,
                    delayL2_, lenL2_);
        fbR_ = half(d + fbL, modR_, static_cast<float>(modR_.length) + depth * lfoQ, delayR1_, lenR1_, dampR_, apR_,
                    delayR2_, lenR2_);

        // Output taps (Table 2): L from the right half, R from the left half, plus one cross tap each.
        const auto& t = taps_;
        const float yl = delayR1_.at(t[0]) + delayR1_.at(t[1]) - apR_.line.at(t[2]) + delayR2_.at(t[3]) -
                         delayL1_.at(t[4]) - apL_.line.at(t[5]) - delayL2_.at(t[6]);
        const float yr = delayL1_.at(t[7]) + delayL1_.at(t[8]) - apL_.line.at(t[9]) + delayL2_.at(t[10]) -
                         delayR1_.at(t[11]) - apR_.line.at(t[12]) - delayR2_.at(t[13]);
        outL[n] = kOutputGain * yl;
        outR[n] = kOutputGain * yr;
    }
}

} // namespace osp::dsp
