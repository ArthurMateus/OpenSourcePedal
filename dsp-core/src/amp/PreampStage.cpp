#include "osp/dsp/amp/PreampStage.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace osp::dsp::amp {

namespace {

// Koren 12AX7 constants (PreampStage.h) and the grid-current clamp: Koren's RGI and a ±0.1 V quadratic knee.
constexpr double kMu = 100.0, kEx = 1.4, kKg1 = 1060.0, kKp = 600.0, kKvb = 300.0;
constexpr double kGridOhms = 2.0e3, kKneeVolts = 0.1;
constexpr double kStep = (PreampStage::kTableMax - PreampStage::kTableMin) / PreampStage::kTableSize;
constexpr double kInvStep = 1.0 / kStep;
constexpr int kZeroNode = static_cast<int>(-PreampStage::kTableMin * kInvStep); // the node at x = 0
constexpr double kFarX = 64.0; // the upper tail matches f here
constexpr float kMaxInput = 1.0e6f; // larger or non-finite input samples are treated as 0

double korenIp(double vgk, double vpk) noexcept
{
    vpk = std::max(vpk, 1.0e-6);
    const double a = kKp * (1.0 / kMu + vgk / std::sqrt(kKvb + vpk * vpk));
    const double softplus = a > 0.0 ? a + std::log1p(std::exp(-a)) : std::log1p(std::exp(a));
    const double e1 = vpk / kKp * softplus;
    return 2.0 * std::pow(e1, kEx) / kKg1; // (E1^Ex + |E1|^Ex) / Kg1 with E1 > 0
}

// Monotone residual r(i) increasing in i on [0, hi]: fixed-count bisection, bounded and noexcept.
template <typename R>
double bisect(R&& r, double hi) noexcept
{
    double lo = 0.0;
    for (int k = 0; k < 64; ++k) {
        const double mid = 0.5 * (lo + hi);
        (r(mid) < 0.0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

float clampSafe(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

// Setters are idempotent: an unchanged value computes nothing (the engine re-sends every parameter every block).
bool changed(float& stored, float v) noexcept
{
    if (v == stored)
        return false;
    stored = v;
    return true;
}

// TPT one-pole coefficient G = g/(1 + g), g = tan(π·f/fs), with f capped below Nyquist.
float tptG(double hz, double fs) noexcept
{
    const double g = std::tan(std::numbers::pi * std::min(hz, 0.45 * fs) / fs);
    return static_cast<float>(g / (1.0 + g));
}

double flush(double s) noexcept
{
    return (s > -1.0e-20 && s < 1.0e-20) ? 0.0 : s;
}

} // namespace

PreampStage::PreampStage() noexcept
{
    drive_.reset(1.0f);
}

void PreampStage::prepare(double sampleRate, const PreampConfig& config) noexcept
{
    sampleRate_ = sampleRate;
    config_ = config;
    auto& c = config_;
    c.supplyVolts = std::clamp(c.supplyVolts, 100.0, 500.0);
    c.plateOhms = std::clamp(c.plateOhms, 10.0e3, 1.0e6);
    c.cathodeOhms = std::clamp(c.cathodeOhms, 100.0, 10.0e3);
    c.sourceOhms = std::clamp(c.sourceOhms, 1.0e3, 1.0e6);
    c.gridLeakOhms = std::clamp(c.gridLeakOhms, 10.0e3, 10.0e6);
    c.cathodeShelfDb = std::clamp(c.cathodeShelfDb, -20.0, 0.0);
    c.outputGainDb = std::clamp(c.outputGainDb, -60.0, 60.0);
    buildTable();
    attackRatio_ = (c.sourceOhms + c.gridLeakOhms) / (c.sourceOhms + kGridOhms);
    conductOffset_ = attackRatio_ / (1.0 + attackRatio_);
    shelfCut_ = 1.0 - std::pow(10.0, c.cathodeShelfDb / 20.0);
    outGain_ = (c.invert ? -1.0 : 1.0) * std::pow(10.0, c.outputGainDb / 20.0);
    drive_.prepare(sampleRate, 0.030);
    for (auto* s : {&gCoupling_, &gConduct_, &gMiller_, &gCathode_})
        s->prepare(sampleRate, 0.020);
    setTargets();
    reset();
}

void PreampStage::buildTable() noexcept
{
    const PreampConfig& c = config_;
    const double rp = c.plateOhms, rk = c.cathodeOhms, b = c.supplyVolts, hi = b / rp;
    const double k = kGridOhms / (c.sourceOhms + kGridOhms);
    // DC bias point (cathode resistor unbypassed at DC), then the AC curve with Vk held at Vk0.
    const double ip0 = bisect([&](double i) { return i - korenIp(-i * rk, b - i * (rp + rk)); }, hi);
    const double vk0 = ip0 * rk;
    auto plateCurrent = [&](double vin) {
        double vgk = vin - vk0;
        const double ramp = vgk <= -kKneeVolts  ? 0.0
                            : vgk >= kKneeVolts ? vgk
                                                : (vgk + kKneeVolts) * (vgk + kKneeVolts) / (4.0 * kKneeVolts);
        vgk -= (1.0 - k) * ramp;
        return bisect([&](double i) { return i - korenIp(vgk, b - i * rp - vk0); }, hi);
    };
    const double ipRest = plateCurrent(0.0);
    const double delta = 1.0e-4 * vk0;
    const double slope = (plateCurrent(delta) - plateCurrent(-delta)) * rp / (2.0 * delta); // V/V
    circuitGainDb_ = 20.0 * std::log10(slope);
    auto normalised = [&](double x) { return (plateCurrent(x * vk0) - ipRest) * rp / (slope * vk0); };

    for (int j = 0; j <= kTableSize; ++j)
        table_[static_cast<std::size_t>(j)].f = j == kZeroNode ? 0.0 : normalised(kTableMin + j * kStep);
    // Exact integrals of the linear interpolant: F1 piecewise quadratic, F2 piecewise cubic. Shifted so that
    // F1(0) = F2(0) = 0 (F1(0) = 0 also makes F2'(0) = 0).
    table_[0].F1 = table_[0].F2 = 0.0;
    for (std::size_t j = 1; j < table_.size(); ++j)
        table_[j].F1 = table_[j - 1].F1 + 0.5 * kStep * (table_[j - 1].f + table_[j].f);
    const double f1Zero = table_[kZeroNode].F1;
    for (auto& node : table_)
        node.F1 -= f1Zero;
    for (std::size_t j = 1; j < table_.size(); ++j) {
        const Node& a = table_[j - 1];
        table_[j].F2 = a.F2 + kStep * (a.F1 + kStep * (a.f / 2.0 + (table_[j].f - a.f) / 6.0));
    }
    const double f2Zero = table_[kZeroNode].F2;
    for (auto& node : table_)
        node.F2 -= f2Zero;

    lowSlope_ = (table_[1].f - table_[0].f) * kInvStep;
    tailY_ = table_[kTableSize].f;
    tailSlope_ = std::max((table_[kTableSize].f - table_[kTableSize - 1].f) * kInvStep, 0.0);
    const double d = kFarX - kTableMax, rise = normalised(kFarX) - tailY_;
    const double den = rise > 0.0 ? tailSlope_ * d / rise - 1.0 : 0.0;
    tailT_ = den > 1.0e-9 ? d / den : 1.0e9; // rational tail y4 + s·d/(1 + d/T); 1e9 = linear
}

const PreampStage::Node& PreampStage::locate(double x, double& frac) const noexcept
{
    const double t = (x - kTableMin) * kInvStep; // kTableMin < x < kTableMax
    const int i = std::min(static_cast<int>(t), kTableSize - 1);
    frac = t - i;
    return table_[static_cast<std::size_t>(i)];
}

double PreampStage::curve(double x) const noexcept
{
    if (x >= kTableMax) {
        const double d = x - kTableMax;
        return tailY_ + tailSlope_ * d / (1.0 + d / tailT_);
    }
    if (!(x > kTableMin)) // also catches NaN
        return table_[0].f + lowSlope_ * (x - kTableMin);
    double frac;
    const Node& a = locate(x, frac);
    const Node& b = (&a)[1];
    return a.f + frac * (b.f - a.f);
}

double PreampStage::curveIntegral(double x) const noexcept
{
    if (x >= kTableMax) {
        const Node& end = table_[kTableSize];
        const double d = x - kTableMax, u = d / tailT_;
        const double g = u < 1.0e-4 ? u * u * (0.5 - u / 3.0) : u - std::log1p(u); // u − ln(1 + u)
        return end.F1 + tailY_ * d + tailSlope_ * tailT_ * tailT_ * g;
    }
    if (!(x > kTableMin)) {
        const double d = x - kTableMin;
        return table_[0].F1 + d * (table_[0].f + 0.5 * lowSlope_ * d);
    }
    double frac;
    const Node& a = locate(x, frac);
    const Node& b = (&a)[1];
    return a.F1 + kStep * frac * (a.f + 0.5 * frac * (b.f - a.f));
}

double PreampStage::curveIntegral2(double x) const noexcept
{
    if (x >= kTableMax) {
        const Node& end = table_[kTableSize];
        const double d = x - kTableMax, u = d / tailT_;
        // integral of (u - ln(1 + u)) = u^2/2 + u - (1 + u) ln(1 + u)
        const double g = u < 1.0e-3 ? u * u * u * (1.0 / 6.0 - u / 12.0 + u * u / 20.0)
                                    : u * u / 2.0 + u - (1.0 + u) * std::log1p(u);
        return end.F2 + d * (end.F1 + 0.5 * tailY_ * d) + tailSlope_ * tailT_ * tailT_ * tailT_ * g;
    }
    if (!(x > kTableMin)) {
        const Node& a = table_[0];
        const double d = x - kTableMin;
        return a.F2 + d * (a.F1 + d * (a.f / 2.0 + lowSlope_ * d / 6.0));
    }
    double frac;
    const Node& a = locate(x, frac);
    const Node& b = (&a)[1];
    return a.F2 + kStep * frac * (a.F1 + kStep * frac * (a.f / 2.0 + (b.f - a.f) * frac / 6.0));
}

void PreampStage::setTargets() noexcept
{
    if (!isPrepared())
        return;
    gCoupling_.setTarget(tptG(couplingHz_, sampleRate_));
    gConduct_.setTarget(tptG(couplingHz_ * (1.0 + attackRatio_), sampleRate_));
    gMiller_.setTarget(tptG(millerHz_, sampleRate_));
    gCathode_.setTarget(tptG(cathodeHz_, sampleRate_));
}

void PreampStage::reset() noexcept
{
    for (auto* s : {&drive_, &gCoupling_, &gConduct_, &gMiller_, &gCathode_})
        s->reset(s->target());
    capState_ = millerState_ = cathodeState_ = 0.0;
    x1_ = x2_ = F2x1_ = d12_ = 0.0; // F1(0) = F2(0) = 0 by construction
}

void PreampStage::setDriveDb(float dB) noexcept
{
    if (changed(driveDb_, clampSafe(dB, -40.0f, 40.0f)))
        drive_.setTarget(std::pow(10.0f, driveDb_ / 20.0f));
}
void PreampStage::setCouplingHz(float hz) noexcept
{
    if (changed(couplingHz_, clampSafe(hz, 1.0f, 500.0f)))
        setTargets();
}
void PreampStage::setMillerHz(float hz) noexcept
{
    if (changed(millerHz_, clampSafe(hz, 1000.0f, 24000.0f)))
        setTargets();
}
void PreampStage::setCathodeHz(float hz) noexcept
{
    if (changed(cathodeHz_, clampSafe(hz, 20.0f, 2000.0f)))
        setTargets();
}

void PreampStage::process(float* io, int numSamples) noexcept
{
    if (!isPrepared()) {
        std::fill_n(io, std::max(numSamples, 0), 0.0f);
        return;
    }
    const bool shelf = shelfCut_ != 0.0;
    for (int n = 0; n < numSamples; ++n) {
        const float in = io[n];
        const double x = static_cast<double>(std::abs(in) <= kMaxInput ? in : 0.0f) * drive_.next();

        // Coupling cap: linear (leak) regime first; if the grid ends above the onset, the grid-current regime.
        const double gc = gCoupling_.next(), go = gConduct_.next();
        double v = (x - capState_) * gc;
        double lp = v + capState_;
        if (x - lp > 1.0) {
            v = (x - conductOffset_ - capState_) * go;
            lp = v + capState_;
        }
        capState_ = flush(lp + v);
        const double grid = x - lp;

        v = (grid - millerState_) * gMiller_.next();
        double u = v + millerState_;
        millerState_ = flush(u + v);

        const double gk = gCathode_.next();
        if (shelf) {
            v = (u - cathodeState_) * gk;
            const double low = v + cathodeState_;
            cathodeState_ = flush(low + v);
            u -= shelfCut_ * low;
        }

        // ADAA2 (Bilbao et al. 2017, as osp::dsp::Adaa2). F2 grows like x², so its differences lose about
        // ε·x²/Δx² of precision: the ill-conditioning tolerance scales with |x| to keep that error near −120 dB.
        const double tol = kAdaaTolerance * (1.0 + std::abs(x1_));
        auto divided = [&](double a, double b, double f2a, double f2b) {
            return std::abs(a - b) < tol ? curveIntegral(0.5 * (a + b)) : (f2a - f2b) / (a - b);
        };
        const double F2 = curveIntegral2(u);
        const double d01 = divided(u, x1_, F2, F2x1_);
        double y;
        if (std::abs(u - x2_) >= tol) {
            y = 2.0 * (d01 - d12_) / (u - x2_);
        } else {
            const double mid = 0.5 * (u + x2_), dm = x1_ - mid;
            y = std::abs(dm) < tol ? curve((u + x1_ + x2_) / 3.0)
                                   : 2.0 * (divided(x1_, mid, F2x1_, curveIntegral2(mid)) - curveIntegral(mid)) / dm;
        }
        x2_ = x1_;
        x1_ = u;
        F2x1_ = F2;
        d12_ = d01;
        io[n] = static_cast<float>(outGain_ * y);
    }
}

double PreampStage::groupDelaySamples(double hz) const noexcept
{
    if (!isPrepared())
        return kDelaySamples;
    auto lowPass = [](double G, std::complex<double> z1) { // TPT one-pole = bilinear: g(1 + z⁻¹)/((1+g) + (g−1)z⁻¹)
        const double g = G / (1.0 - G);
        return g * (1.0 + z1) / ((1.0 + g) + (g - 1.0) * z1);
    };
    auto response = [&](double f) {
        const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / sampleRate_);
        std::complex<double> h = (1.0 - lowPass(gCoupling_.target(), z1)) * lowPass(gMiller_.target(), z1);
        if (shelfCut_ != 0.0)
            h *= 1.0 - shelfCut_ * lowPass(gCathode_.target(), z1);
        return h * (1.0 + z1 + z1 * z1) / 3.0; // ADAA2 on a linear segment
    };
    constexpr double kDelta = 0.5;
    const double dw = 2.0 * std::numbers::pi * 2.0 * kDelta / sampleRate_;
    return -std::arg(response(hz + kDelta) / response(hz - kDelta)) / dw;
}

int PreampStage::getLatencySamples() const noexcept
{
    return static_cast<int>(std::lround(getMeasuredDelaySamples()));
}

} // namespace osp::dsp::amp
