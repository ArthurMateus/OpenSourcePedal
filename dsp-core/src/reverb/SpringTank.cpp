#include "osp/dsp/reverb/SpringTank.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace osp::dsp {
namespace {

// Voicing (see the header): allpass coefficients, transit times per spring, in-loop damping, output levels.
constexpr float kLfCoeff = 0.6f, kHfCoeff = -0.6f;
constexpr std::array<double, 2> kLfTransitMs{41.0, 47.3};
constexpr std::array<double, 2> kHfTransitMs{17.9, 21.7};
// Jot's damping one-pole per loop (J. O. Smith, PASP, "Achieving Desired Reverberation Times", CCRMA, accessed
// 2026-10-27): p = ln(10)/4 · log10(g) · (1 − 1/α²), with α = RT60(fs/2) / RT60(0). It scales with the loop loss, so
// the compensated gain never reaches 1 at low frequencies (a fixed 3.8 kHz one-pole made 15 s decays ring 67 % long).
constexpr double kAlpha = 0.5, kMaxPole = 0.8;
constexpr float kLfLevel = 0.5f, kHfLevel = 0.1f; // C_hf at −14 dB
constexpr double kExcursion48k = 6.0; // delay-read excursion at 100 % modulation, samples at 48 kHz
constexpr double kLfoHz = 0.7;
constexpr double kMidHz = 707.1; // RT60 reference, as in FdnTank
constexpr double kMaxLoopGain = 0.99999; // below lossless outside freeze

// Exact group delay (samples) of M cascaded A(z^k) = (a + z^−k)/(1 + a·z^−k) at w rad/sample.
double cascadeGroupDelay(int stages, int k, double a, double w) noexcept
{
    return stages * k * (1.0 - a * a) / (1.0 + 2.0 * a * std::cos(k * w) + a * a);
}

// The loops limit-cycle just above the smallest normal float (about 1.3e-38, measured), where FTZ does not reach, so
// the feedback snaps at −400 dB, as SvfState does.
float snap(float v) noexcept
{
    return std::abs(v) < 1.0e-20f ? 0.0f : v;
}

} // namespace

float SpringTank::Line::frac(float delay) const noexcept
{
    const int i = static_cast<int>(delay);
    const float f = delay - static_cast<float>(i);
    const int size = static_cast<int>(buf.size());
    int r0 = write - i, r1 = r0 - 1;
    r0 += r0 < 0 ? size : 0;
    r1 += r1 < 0 ? size : 0;
    const float a = buf[static_cast<std::size_t>(r0)], b = buf[static_cast<std::size_t>(r1)];
    return a + f * (b - a);
}

void SpringTank::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    stretch_ = std::max(1, static_cast<int>(std::lround(sampleRate / (2.0 * kTransitionHz))));
    excursion_ = static_cast<float>(kExcursion48k * sampleRate / 48000.0);
    const int margin = static_cast<int>(std::ceil(excursion_)) + 2;
    const double w = 2.0 * std::numbers::pi * kMidHz / sampleRate;
    for (std::size_t c = 0; c < springs_.size(); ++c) {
        Spring& s = springs_[c];
        s.lfState.assign(static_cast<std::size_t>(kLfStages * stretch_), 0.0f);
        s.lfLength = std::max(2, static_cast<int>(std::lround(kLfTransitMs[c] * 1e-3 * sampleRate)));
        s.hfLength = std::max(2, static_cast<int>(std::lround(kHfTransitMs[c] * 1e-3 * sampleRate)));
        s.lfDelay.buf.assign(static_cast<std::size_t>(s.lfLength + margin + 1), 0.0f);
        s.hfDelay.buf.assign(static_cast<std::size_t>(s.hfLength + margin + 1), 0.0f);
        s.lfLoop = s.lfLength + cascadeGroupDelay(kLfStages, stretch_, kLfCoeff, w);
        s.hfLoop = s.hfLength + cascadeGroupDelay(kHfStages, 1, kHfCoeff, w);
    }
    outLp_ = svfDesign(SvfType::lowPass, sampleRate, sampleRate / (2.0 * stretch_), 0.70710678, 0.0);
    lfoInc_ = 2.0 * std::numbers::pi * kLfoHz / sampleRate;
    decay_.prepare(sampleRate, 0.020);
    mod_.prepare(sampleRate, 0.020);
    freeze_.prepare(sampleRate, 0.050);
    reset();
}

void SpringTank::reset() noexcept
{
    for (Spring& s : springs_) {
        std::fill(s.lfState.begin(), s.lfState.end(), 0.0f);
        s.hfState.fill(0.0f);
        for (Line* l : {&s.lfDelay, &s.hfDelay}) {
            std::fill(l->buf.begin(), l->buf.end(), 0.0f);
            l->write = 0;
        }
        s.lfDamp = s.hfDamp = s.lfFb = s.hfFb = 0.0f;
        s.outLp = {};
    }
    lfIndex_ = 0;
    lfoPhase_ = 0.0;
    decay_.reset(decay_.target());
    mod_.reset(mod_.target());
    freeze_.reset(freeze_.target());
    cachedDecay_ = -1.0f;
    updateGains();
}

void SpringTank::updateGains() noexcept
{
    const float rt60 = std::clamp(decay_.current(), 0.3f, 20.0f);
    if (rt60 == cachedDecay_ || sampleRate_ <= 0.0)
        return;
    cachedDecay_ = rt60;
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * kMidHz / sampleRate_);
    // Loop gain g and Jot pole p, with the one-pole's loss at 707 Hz, |(1 − p)/(1 − p·z⁻¹)|, compensated.
    auto design = [&](double loopSamples, float& gain, float& pole) {
        const double g = std::pow(10.0, -3.0 * loopSamples / (sampleRate_ * rt60));
        const double p =
            std::clamp(std::log(10.0) / 4.0 * std::log10(g) * (1.0 - 1.0 / (kAlpha * kAlpha)), 0.0, kMaxPole);
        gain = static_cast<float>(std::min(g * std::abs(1.0 - p * z) / (1.0 - p), kMaxLoopGain));
        pole = static_cast<float>(p);
    };
    for (Spring& s : springs_) {
        design(s.lfLoop, s.lfGain, s.lfPole);
        design(s.hfLoop, s.hfGain, s.hfPole);
    }
}

void SpringTank::process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    if (sampleRate_ <= 0.0) {
        std::fill(outL, outL + numSamples, 0.0f);
        std::fill(outR, outR + numSamples, 0.0f);
        return;
    }
    const auto k = static_cast<std::size_t>(stretch_);
    float* const out[2] = {outL, outR};
    for (int n = 0; n < numSamples; ++n) {
        decay_.next();
        updateGains(); // recomputes only while the decay smoother moves (parameters.md §1.4: per sample)
        const float frz = freeze_.next();
        const float depth = excursion_ * mod_.next() * (1.0f - frz);
        const float x = 0.5f * (inL[n] + inR[n]) * (1.0f - frz);
        const float lfo[2] = {static_cast<float>(std::sin(lfoPhase_)), static_cast<float>(std::cos(lfoPhase_))};
        lfoPhase_ += lfoInc_;
        if (lfoPhase_ > 2.0 * std::numbers::pi)
            lfoPhase_ -= 2.0 * std::numbers::pi;

        // The four cascades (C_lf and C_hf of both springs) run side by side: each is a serial chain of allpasses, so
        // interleaving them gives the CPU independent work (bit-identical to running them one after another).
        // Canonical form of A(z^k): w = u − a·w[n−k], y = a·w + w[n−k]; the C_lf rings share one index.
        float u[2] = {x + springs_[0].lfFb, x + springs_[1].lfFb};
        float v[2] = {x + springs_[0].hfFb, x + springs_[1].hfFb};
        float* lf[2] = {springs_[0].lfState.data() + lfIndex_, springs_[1].lfState.data() + lfIndex_};
        auto stage = [](float& in, float& state, float a) {
            const float w = in - a * state;
            in = a * w + state;
            state = w;
        };
        for (std::size_t m = 0; m < static_cast<std::size_t>(kHfStages); ++m) {
            stage(u[0], lf[0][m * k], kLfCoeff);
            stage(u[1], lf[1][m * k], kLfCoeff);
            stage(v[0], springs_[0].hfState[m], kHfCoeff);
            stage(v[1], springs_[1].hfState[m], kHfCoeff);
        }
        for (std::size_t m = kHfStages; m < static_cast<std::size_t>(kLfStages); ++m) {
            stage(u[0], lf[0][m * k], kLfCoeff);
            stage(u[1], lf[1][m * k], kLfCoeff);
        }

        for (std::size_t c = 0; c < 2; ++c) {
            Spring& s = springs_[c];
            s.lfDelay.push(u[c]);
            const float rl = s.lfDelay.frac(static_cast<float>(s.lfLength) + depth * lfo[c]);
            s.lfDamp = (1.0f - s.lfPole) * rl + s.lfPole * s.lfDamp;
            const float gl = s.lfGain + frz * (1.0f - s.lfGain);
            s.lfFb = snap(gl * (s.lfDamp + frz * (rl - s.lfDamp))); // damping off while frozen

            s.hfDelay.push(v[c]);
            const float rh = s.hfDelay.frac(static_cast<float>(s.hfLength) + depth * lfo[1 - c]);
            s.hfDamp = (1.0f - s.hfPole) * rh + s.hfPole * s.hfDamp;
            const float gh = s.hfGain + frz * (1.0f - s.hfGain);
            s.hfFb = snap(gh * (s.hfDamp + frz * (rh - s.hfDamp)));

            out[c][n] = kLfLevel * s.outLp.tick(outLp_, rl) + kHfLevel * rh;
        }
        if (++lfIndex_ == stretch_)
            lfIndex_ = 0;
    }
}

} // namespace osp::dsp
