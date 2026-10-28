#include "osp/dsp/amp/PowerAmp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace osp::dsp::amp {

namespace {

constexpr double kTolerance = 1e-5; // ADAA1 midpoint fallback below this |Δγ| (ADR-0005 D1, kAdaaTolerance)
constexpr double kMaxInput = 1.0e3; // larger inputs are clamped; non-finite ones become 0

double clampKnob(float knob) noexcept
{
    return knob >= 0.0f ? (knob <= 10.0f ? knob : 10.0) : 0.0; // NaN -> 0
}

} // namespace

PowerAmp::PowerAmp() noexcept
{
    presenceK_.reset(5.0f); // parameters.md §2.5 defaults
    depthK_.reset(5.0f);
    masterK_.reset(5.0f);
}

double PowerAmp::Section::tick(double x) noexcept
{
    const double y = b0 * x + s;
    s = b1 * x - a1 * y;
    if (std::abs(s) < 1.0e-30)
        s = 0.0;
    return y;
}

std::complex<double> PowerAmp::Section::at(std::complex<double> zInv) const noexcept
{
    return (b0 + b1 * zInv) / (1.0 + a1 * zInv);
}

// Bilinear 1st-order section of H(s) = (n1·s + n0)/(d1·s + d0), s normalised to the midpoint, warp = 1/tan(ωc/2fs).
// Closed-loop target C (gain G = r², midpoint 1): high shelf (1 + s·r)/(1 + s/r), low shelf (s + r)/(s + 1/r).
// β section S = ((1 + N)/C − 1)/N, with c = ((1 + N)/r − r)/N:
//   high: (1 + c·s)/(1 + r·s)       low: (s + c)/(s + r)
PowerAmp::Section PowerAmp::shelf(bool high, double knob, double n, double warp) noexcept
{
    const double r = std::pow(10.0, (knob - 5.0) / 20.0); // sqrt of the closed-loop gain (knob − 5)·2 dB
    const double c = std::max(0.0, ((1.0 + n) / r - r) / n);
    const double n1 = high ? c : 1.0, n0 = high ? 1.0 : c, d1 = high ? r : 1.0, d0 = high ? 1.0 : r;
    const double a0 = d1 * warp + d0;
    Section s;
    s.b0 = (n1 * warp + n0) / a0;
    s.b1 = (n0 - n1 * warp) / a0;
    s.a1 = (d0 - d1 * warp) / a0;
    return s;
}

void PowerAmp::updateShelf(Section& section, bool high, float knob) noexcept
{
    const double state = section.s;
    section = shelf(high, knob, n_, high ? warpP_ : warpD_);
    section.s = state;
}

void PowerAmp::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    fs_ = sampleRate > 0.0 ? sampleRate : 0.0;
    if (fs_ <= 0.0)
        return;
    const auto& v = voicing_;
    beam_ = v.shape == PowerAmpShape::beam;
    n_ = std::pow(10.0, std::clamp(v.nfbDb, 6.0, 20.0) / 20.0);
    l0_ = v.ceiling > 1e-3 ? v.ceiling : 1e-3;
    sag_ = std::clamp(v.sagAmount, 0.0, 0.5);
    auto coeff = [&](double ms) { return 1.0 - std::exp(-1.0 / (std::max(ms, 0.1) * 1e-3 * fs_)); };
    sagAttack_ = coeff(v.sagAttackMs);
    sagRelease_ = coeff(v.sagReleaseMs);
    iterations_ = std::clamp(v.maxNewtonIterations, 1, 64);
    auto warp = [&](double hz) { return 1.0 / std::tan(std::numbers::pi * std::min(hz, 0.45 * fs_) / fs_); };
    warpP_ = warp(kPresenceHz);
    warpD_ = warp(kDepthHz);
    const double kh = warp(std::max(v.loopHpHz, 0.1)); // HPF s/(s + 1)
    hp_.b0 = kh / (kh + 1.0);
    hp_.b1 = -hp_.b0;
    hp_.a1 = (1.0 - kh) / (1.0 + kh);
    hpOut_ = hp_;
    postHpOn_ = v.otHpHz > 0.0;
    postLpOn_ = v.otLpHz > 0.0;
    if (postHpOn_)
        postHpC_ = svfDesign(SvfType::highPass, fs_, v.otHpHz, std::numbers::sqrt2 / 2.0, 0.0);
    if (postLpOn_)
        postLpC_ = svfDesign(SvfType::lowPass, fs_, v.otLpHz, std::max(v.otLpQ, 0.1), 0.0);
    presenceK_.prepare(fs_, 0.020);
    depthK_.prepare(fs_, 0.020);
    masterK_.prepare(fs_, 0.030);
    reset();
}

void PowerAmp::reset() noexcept
{
    presenceK_.reset(presenceK_.target());
    depthK_.reset(depthK_.target());
    masterK_.reset(masterK_.target());
    if (!isPrepared())
        return;
    hp_.s = hpOut_.s = presence_.s = depth_.s = 0.0;
    updateShelf(presence_, true, presenceK_.current());
    updateShelf(depth_, false, depthK_.current());
    updateMaster(masterK_.current());
    postHp_ = {};
    postLp_ = {};
    l_ = l0_;
    gamma1_ = g1_ = phi1_ = 0.0; // Φ(0) = 0, G(0) = 0
    k1_ = n_ * hp_.b0 * presence_.b0 * depth_.b0;
    droop_ = 0.0;
}

void PowerAmp::setPresence(float knob) noexcept
{
    presenceK_.setTarget(static_cast<float>(clampKnob(knob)));
}

void PowerAmp::setDepth(float knob) noexcept
{
    depthK_.setTarget(static_cast<float>(clampKnob(knob)));
}

void PowerAmp::setMaster(float knob) noexcept
{
    masterK_.setTarget(static_cast<float>(clampKnob(knob)));
}

void PowerAmp::updateMaster(float knob) noexcept
{
    const double db = voicing_.masterDbAt5 + (knob - 5.0) * voicing_.masterDbPerKnob;
    gIn_ = std::pow(10.0, db / 20.0) * (1.0 + n_) / n_;
}

// The history γ1 is stored in units of L. A new L moves it to γ1·L/L'; G there is G(γ1) + Φ(γ1)·Δγ to first order.
void PowerAmp::setCeiling(double l) noexcept
{
    const double moved = gamma1_ * l_ / l;
    g1_ += phi1_ * (moved - gamma1_);
    gamma1_ = moved;
    l_ = l;
}

void PowerAmp::loopMap(double gamma, double k, double& phi, double& g) const noexcept
{
    const double a = std::abs(gamma);
    double w = 0.0, f = 0.0, F = 0.0;
    if (beam_) {
        w = std::max(a / (1.0 + k), a - k); // left of the root (tanh x ≤ x, tanh x < 1)
        for (int it = 0; it < iterations_; ++it) {
            const double t = std::tanh(w);
            const double step = (a - w - k * t) / (1.0 + k * (1.0 - t * t)); // ≥ 0: monotone from the left
            w += step;
            if (!(step > 1e-13 * (1.0 + w)))
                break;
        }
        const double e = std::exp(-2.0 * w);
        f = (1.0 - e) / (1.0 + e);
        F = w + std::log1p(e) - std::numbers::ln2; // log cosh, overflow-safe (ADR-0005 D1)
    } else {
        // ω + K·ω/(1 + ω) = a  <=>  ω² + (1 + K − a)·ω − a = 0, positive root in a cancellation-free form.
        const double b = a - 1.0 - k, s = std::sqrt(b * b + 4.0 * a);
        w = b >= 0.0 ? 0.5 * (b + s) : 2.0 * a / (s - b);
        f = w / (1.0 + w);
        F = w - std::log1p(w);
    }
    phi = gamma < 0.0 ? -f : f;
    g = F + 0.5 * k * f * f; // even in γ
}

void PowerAmp::process(float* io, int numSamples) noexcept
{
    if (!isPrepared()) {
        std::fill(io, io + std::max(numSamples, 0), 0.0f);
        return;
    }
    for (int i = 0; i < numSamples; ++i) {
        if (presenceK_.isSmoothing())
            updateShelf(presence_, true, presenceK_.next());
        if (depthK_.isSmoothing())
            updateShelf(depth_, false, depthK_.next());
        if (masterK_.isSmoothing())
            updateMaster(masterK_.next());

        double x = io[i];
        if (!(std::abs(x) <= kMaxInput))
            x = std::isfinite(x) ? std::copysign(kMaxInput, x) : 0.0;
        // B(v) = B0·v + c0 through HPF -> presence -> depth; c0 from their stored states.
        const double c0 = depth_.b0 * (presence_.b0 * hp_.s + presence_.s) + depth_.s;
        const double k = n_ * hp_.b0 * presence_.b0 * depth_.b0;
        if (k != k1_) { // G(γ1) at the new K: dG/dK = −Φ²/2
            g1_ -= 0.5 * phi1_ * phi1_ * (k - k1_);
            k1_ = k;
        }
        const double gamma = n_ * (gIn_ * x - c0) / l_;
        double phi = 0.0, g = 0.0;
        loopMap(gamma, k, phi, g);
        const double dg = gamma - gamma1_;
        double v = l_ * (std::abs(dg) < kTolerance ? 0.5 * (phi + phi1_) : (g - g1_) / dg);
        gamma1_ = gamma;
        g1_ = g;
        phi1_ = phi;
        if (!(std::abs(v) <= 2.0 * l0_)) // unreachable in exact arithmetic (|v| < L); keeps RT-9 if it ever is
            v = 0.0;

        depth_.tick(presence_.tick(hp_.tick(l_ * phi))); // the loop runs on the instantaneous solution
        const double y = hpOut_.tick(v);                  // the output tap takes the ADAA1 output

        if (sag_ > 0.0) { // supply rail RC: droop toward S·i under load, recharge otherwise
            const double target = sag_ * std::min(std::abs(v) / l0_, 1.0);
            droop_ += (target - droop_) * (target > droop_ ? sagAttack_ : sagRelease_);
            setCeiling(l0_ * (1.0 - droop_));
        }

        float out = static_cast<float>(y);
        if (postHpOn_)
            out = postHp_.tick(postHpC_, out);
        if (postLpOn_)
            out = postLp_.tick(postLpC_, out);
        io[i] = out;
    }
}

std::complex<double> PowerAmp::loopGain(double hz, bool unitDelay) const noexcept
{
    if (!isPrepared())
        return {};
    const auto zInv = std::polar(1.0, -2.0 * std::numbers::pi * hz / fs_);
    const auto t = n_ * hp_.at(zInv) * shelf(true, presenceK_.target(), n_, warpP_).at(zInv) *
                   shelf(false, depthK_.target(), n_, warpD_).at(zInv);
    return unitDelay ? t * zInv : t;
}

std::complex<double> PowerAmp::response(double hz) const noexcept
{
    if (!isPrepared())
        return {};
    const auto zInv = std::polar(1.0, -2.0 * std::numbers::pi * hz / fs_);
    const double db = voicing_.masterDbAt5 + (masterK_.target() - 5.0) * voicing_.masterDbPerKnob;
    const double gIn = std::pow(10.0, db / 20.0) * (1.0 + n_) / n_;
    // The loop solves Φ = N·u/(1 + T) exactly; the output is ADAA1 of it (h = (1 + z⁻¹)/2) through HPF_loop.
    auto r = gIn * n_ * 0.5 * (1.0 + zInv) * hp_.at(zInv) / (1.0 + loopGain(hz));
    // Post filters: the TPT SVF is the bilinear transform prewarped at its corner, s = (1/g)(1 − z⁻¹)/(1 + z⁻¹).
    auto svf = [&](double fc, double q, bool high) {
        const double g = std::tan(std::numbers::pi * std::min(fc, 0.49 * fs_) / fs_);
        const auto s = (1.0 - zInv) / (g * (1.0 + zInv));
        return (high ? s * s : std::complex<double>(1.0)) / (s * s + s / q + 1.0);
    };
    if (postHpOn_)
        r *= svf(voicing_.otHpHz, std::numbers::sqrt2 / 2.0, true);
    if (postLpOn_)
        r *= svf(voicing_.otLpHz, std::max(voicing_.otLpQ, 0.1), false);
    return r;
}

} // namespace osp::dsp::amp
