#include "osp/dsp/amp/ToneStack.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace osp::dsp::amp {

namespace {

constexpr double kPi = 3.14159265358979323846;
using Complex = std::complex<double>;

// The zero zeta (|zeta| <= 1) whose |1 - zeta e^-jw|^2 is proportional to (y - Y): zeta^2 - 2(1 - Y) zeta + 1 = 0.
// The two roots multiply to 1, so the small one is 1 / the large one (no cancellation).
Complex zeroFromRoot(Complex root) noexcept
{
    if (!(std::abs(root) < 1e100))
        return {0.0, 0.0};
    const Complex x = 1.0 - root, s = std::sqrt(root * (root - 2.0));
    const Complex p = x + s, m = x - s;
    return 1.0 / (std::abs(p) >= std::abs(m) ? p : m);
}

// Least squares min |A x - r| for an N x 3 system by Householder QR (A and r are overwritten).
template <std::size_t N>
bool solveLeastSquares3(std::array<std::array<double, 3>, N>& a, std::array<double, N>& r, double x[3]) noexcept
{
    for (std::size_t j = 0; j < 3; ++j) {
        double norm = 0.0;
        for (std::size_t i = j; i < N; ++i)
            norm += a[i][j] * a[i][j];
        norm = std::sqrt(norm);
        if (!(norm > 0.0))
            return false;
        const double alpha = a[j][j] > 0.0 ? -norm : norm;
        a[j][j] -= alpha; // column j below the diagonal is now the Householder vector v
        double vv = 0.0;
        for (std::size_t i = j; i < N; ++i)
            vv += a[i][j] * a[i][j];
        auto reflect = [&](auto get) {
            double dot = 0.0;
            for (std::size_t i = j; i < N; ++i)
                dot += a[i][j] * get(i);
            return 2.0 * dot / vv;
        };
        for (std::size_t c = j + 1; c < 3; ++c) {
            const double f = reflect([&](std::size_t i) { return a[i][c]; });
            for (std::size_t i = j; i < N; ++i)
                a[i][c] -= f * a[i][j];
        }
        const double f = reflect([&](std::size_t i) { return r[i]; });
        for (std::size_t i = j; i < N; ++i)
            r[i] -= f * a[i][j];
        a[j][j] = alpha; // R's diagonal
    }
    for (int j = 2; j >= 0; --j) {
        double v = r[static_cast<std::size_t>(j)];
        for (int c = j + 1; c < 3; ++c)
            v -= a[static_cast<std::size_t>(j)][static_cast<std::size_t>(c)] * x[c];
        x[j] = v / a[static_cast<std::size_t>(j)][static_cast<std::size_t>(j)];
    }
    return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

} // namespace

const ToneStackComponents& toneStackComponents(ToneStackVariant variant) noexcept
{
    return variant == ToneStackVariant::b ? kToneStackB : variant == ToneStackVariant::c ? kToneStackC : kToneStackA;
}

double toneStackPotFraction(double knob, bool audioTaper, bool rheostat) noexcept
{
    const double x = std::isnan(knob) ? 0.0 : std::clamp(knob / 10.0, 0.0, 1.0);
    const double f = audioTaper ? (std::pow(81.0, x) - 1.0) / 80.0 : x;
    return rheostat ? kMinRheostat + (1.0 - kMinRheostat) * f : f; // end resistance in series: no kink
}

// Transcribed from tools/amp/tonestack_derive.py (own nodal analysis), grouped by knob monomials.
ToneStackAnalog toneStackAnalog(const ToneStackComponents& k, double t, double l, double m) noexcept
{
    const double rs = k.rSource, r1 = k.rTreble, r2 = k.rBass, r3 = k.rMid, r4 = k.rSlope, rl = k.rLoad;
    const double c1 = k.cTreble, c2 = k.cBass, c3 = k.cMid;
    const double c12 = c1 + c2, c23 = c2 + c3, ct = c1 + c2 + c3;
    const double e = c1 * r1 + c23 * r4 + ct * rs;      // recurring a1 factor
    const double p3 = r1 * r4 + r1 * rs + r4 * rs;      // recurring a2/a3 factor
    const double tt = t * t;

    ToneStackAnalog h;
    h.b[1] = rl * (t * c1 * r1 + l * r2 * c12 + m * r3 * ct);
    h.b[2] = rl * (t * c1 * r1 * r4 * c23 + l * m * c3 * r2 * r3 * c12 + l * c1 * r2 * (c2 * r1 + c23 * r4) +
                   m * c1 * r3 * c23 * (r1 + r4));
    h.b[3] = rl * c1 * c2 * c3 * r2 * l * (t * r1 * r4 + m * r3 * (r1 + r4));

    h.a[0] = t * r1 + l * r2 + m * r3 + rl;
    h.a[1] = -tt * c1 * r1 * r1 - t * l * r1 * r2 * (c1 - c2) - t * m * r1 * r3 * (c1 - c23) + t * r1 * e +
             l * m * c3 * r2 * r3 + l * r2 * (e + c12 * rl) + m * r3 * (e + ct * rl) + rl * e;
    h.a[2] = -tt * l * c1 * c2 * r1 * r1 * r2 - tt * m * c1 * r1 * r1 * r3 * c23 - tt * c1 * r1 * r1 * c23 * (r4 + rs) -
             t * l * m * c3 * r1 * r2 * r3 * (c1 - c2) +
             t * l * r1 * r2 * (c1 * c2 * (r1 - r4) + (c2 - c1) * c3 * (r4 + rs)) +
             t * m * c1 * r1 * r3 * c23 * (r1 - r4) + t * c1 * r1 * c23 * p3 +
             l * m * c3 * r2 * r3 * (c1 * (r1 + rl + rs) + c2 * (r4 + rl + rs)) +
             l * r2 *
                 (c1 * c2 * (r1 * r4 + r1 * rl + r1 * rs + r4 * rl + r4 * rs) +
                  c1 * c3 * (r1 * r4 + r1 * rs + r4 * rl + r4 * rs + rl * rs) + c2 * c3 * rl * (r4 + rs)) +
             m * c1 * r3 * c23 * (p3 + r1 * rl + r4 * rl) + c1 * rl * c23 * p3;
    h.a[3] = c1 * c2 * c3 * l * r2 *
             (-tt * m * r1 * r1 * r3 - tt * r1 * r1 * (r4 + rs) + t * m * r1 * r3 * (r1 - r4) + t * r1 * p3 +
              m * r3 * (p3 + r1 * rl + r4 * rl) + rl * p3);
    return h;
}

void ToneStackDesigner::prepare(double sampleRate) noexcept
{
    fs_ = sampleRate > 0.0 ? sampleRate : 0.0;
    const double top = std::min(fs_ < 80000.0 ? 16000.0 : 20000.0, 0.45 * fs_);
    for (int k = 0; k < kFitPoints; ++k) {
        const double f = k < 10 ? 20.0 * std::pow(0.5 * top / 20.0, k / 9.0) : 0.5 * top * (1.0 + (k - 9) / 16.0);
        const double s = std::sin(kPi * f / std::max(fs_, 1.0));
        omega_[static_cast<std::size_t>(k)] = 2.0 * kPi * f;
        y_[static_cast<std::size_t>(k)] = 2.0 * s * s;
    }
}

bool ToneStackDesigner::design(
    const ToneStackComponents& k, double bass, double mid, double treble, ToneStackCoeffs& out) const noexcept
{
    if (!(fs_ > 0.0))
        return false;
    const double t = toneStackPotFraction(treble, k.trebleAudio, false);
    const double l = toneStackPotFraction(bass, k.bassAudio, true);
    const double m = toneStackPotFraction(mid, k.midAudio, true);
    const ToneStackAnalog h = toneStackAnalog(k, t, l, m);
    const auto& a = h.a;
    const auto& b = h.b;

    // 1. Poles: Newton from s = 0 converges monotonically to the root nearest 0; deflate, then a stable quadratic.
    double r1 = 0.0;
    for (int it = 0; it < 100; ++it) {
        const double v = ((a[3] * r1 + a[2]) * r1 + a[1]) * r1 + a[0];
        const double dv = (3.0 * a[3] * r1 + 2.0 * a[2]) * r1 + a[1];
        const double ds = v / dv;
        if (!std::isfinite(ds))
            return false;
        r1 -= ds;
        if (std::abs(ds) <= 1e-15 * std::abs(r1))
            break;
    }
    if (!(r1 < 0.0))
        return false;
    const double q0 = -a[0] / r1, q1 = (q0 - a[1]) / r1; // a(s) = (s - r1)(a3 s^2 + q1 s + q0)
    const double qq = -0.5 * (q1 + std::sqrt(std::max(0.0, q1 * q1 - 4.0 * a[3] * q0)));
    const double roots[3] = {r1, q0 / qq, a[3] > 0.0 ? qq / a[3] : -1e300};
    double z[3], oneMinusZ[3];
    for (int i = 0; i < 3; ++i) {
        const double pt = std::max(roots[i] / fs_, -700.0);
        if (!(pt < 0.0))
            return false;
        z[i] = std::exp(pt);
        oneMinusZ[i] = -std::expm1(pt);
    }

    // 2./3. Weighted LS fit of P(y) = alpha + beta y + gamma y^2 to G2 = |H(jW)|^2 |A(z)|^2 / |1 - z^-1|^2.
    std::array<double, kFitPoints> g2{}, w{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFitPoints); ++i) {
        const double om = omega_[i], om2 = om * om, y = y_[i];
        const double nr = -b[2] * om2, ni = b[1] * om - b[3] * om * om2;
        const double dr = a[0] - a[2] * om2, di = a[1] * om - a[3] * om * om2;
        double a2 = 1.0;
        for (int p = 0; p < 3; ++p)
            a2 *= oneMinusZ[p] * oneMinusZ[p] + 2.0 * z[p] * y;
        g2[i] = (nr * nr + ni * ni) / (dr * dr + di * di) * a2 / (2.0 * y);
        if (!(g2[i] > 0.0) || !std::isfinite(g2[i]))
            return false;
        w[i] = 1.0;
    }
    double coef[3] = {0.0, 0.0, 0.0};
    for (int pass = 0; pass <= kLawsonPasses; ++pass) {
        std::array<std::array<double, 3>, kFitPoints> mat{};
        std::array<double, kFitPoints> rhs{};
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFitPoints); ++i) {
            const double s = w[i] / g2[i];
            mat[i] = {s, s * y_[i], s * y_[i] * y_[i]};
            rhs[i] = w[i];
        }
        if (!solveLeastSquares3(mat, rhs, coef))
            return false;
        if (pass == kLawsonPasses)
            break;
        double errMax = 0.0;
        std::array<double, kFitPoints> err{};
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFitPoints); ++i) {
            err[i] = std::abs((coef[0] + (coef[1] + coef[2] * y_[i]) * y_[i]) / g2[i] - 1.0);
            errMax = std::max(errMax, err[i]);
        }
        if (!(errMax > 1e-12))
            break;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFitPoints); ++i)
            w[i] *= std::sqrt(err[i] / errMax + 1e-3);
    }

    // Spectral factorisation: the roots Y of P(y) give the zeros of the minimum-phase B'.
    const double alpha = coef[0], beta = coef[1], gamma = coef[2];
    const Complex d = std::sqrt(Complex(beta * beta - 4.0 * alpha * gamma, 0.0));
    const Complex qy = -0.5 * (beta + (beta * d.real() >= 0.0 ? d : -d));
    if (std::abs(qy) == 0.0)
        return false;
    const Complex y1 = gamma != 0.0 ? qy / gamma : Complex(1e300, 0.0), y2 = alpha / qy;
    const Complex zeta1 = zeroFromRoot(y1), zeta2 = zeroFromRoot(y2);
    const double p = -(zeta1 + zeta2).real(), q = (zeta1 * zeta2).real();
    // Gain: P(y) / |1 + p e^-jw + q e^-2jw|^2 is constant; evaluate it at the top fit point.
    const double yr = y_[static_cast<std::size_t>(kFitPoints - 1)];
    const double u = (1.0 + p + q) * (1.0 + p + q) - 2.0 * (p + p * q + 4.0 * q) * yr + 4.0 * q * yr * yr;
    const double gain2 = (alpha + (beta + gamma * yr) * yr) / u;
    if (!(gain2 > 0.0) || !std::isfinite(gain2) || !std::isfinite(p) || !std::isfinite(q))
        return false;
    const double c0 = std::sqrt(gain2), c1 = c0 * p, c2 = c0 * q;

    // Modal form: H(u) - c0 = u Q(u) / D(u) with u = z^-1, D(u) = prod (1 - z_i u); residue r_i at u = 1/z_i.
    const double e1 = z[0] + z[1] + z[2], e2 = z[0] * z[1] + z[0] * z[2] + z[1] * z[2], e3 = z[0] * z[1] * z[2];
    const double qa = c1 - c0 + c0 * e1, qb = c2 - c1 - c0 * e2, qc = c0 * e3 - c2;
    out.d = c0;
    for (int i = 0; i < 3; ++i) {
        const double zi = z[i];
        double den = 1.0;
        for (int j = 0; j < 3; ++j)
            if (j != i)
                den *= zi - z[j];
        out.r[static_cast<std::size_t>(i)] = ((qa * zi + qb) * zi + qc) / den;
        out.z[static_cast<std::size_t>(i)] = zi;
    }
    return std::isfinite(out.r[0]) && std::isfinite(out.r[1]) && std::isfinite(out.r[2]);
}

ToneStack::ToneStack() noexcept : components_(kToneStackA)
{
    bass_.reset(5.0f);
    mid_.reset(5.0f);
    treble_.reset(5.0f);
}

void ToneStack::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    designer_.prepare(sampleRate);
    bass_.prepare(sampleRate, kSmoothingSeconds);
    mid_.prepare(sampleRate, kSmoothingSeconds);
    treble_.prepare(sampleRate, kSmoothingSeconds);
    interval_ = std::max(1, static_cast<int>(std::lround(sampleRate * kUpdateSeconds)));
    cur_ = {};
    reset();
}

void ToneStack::reset() noexcept
{
    clearState();
    bass_.reset(bass_.target());
    mid_.reset(mid_.target());
    treble_.reset(treble_.target());
    jump_ = true;
    dirty_ = true;
}

void ToneStack::clearState() noexcept
{
    x1_ = 0.0;
    w_ = {};
}

void ToneStack::setVariant(ToneStackVariant variant) noexcept
{
    setComponents(toneStackComponents(variant));
}

void ToneStack::setComponents(const ToneStackComponents& components) noexcept
{
    components_ = components;
    dirty_ = true;
}

// The pot positions in a log-resistance view: the poles move roughly as 1/R near a pot's end, so equal steps here are
// about equal steps of the network.
std::array<double, 4> ToneStack::logPots(double bass, double mid, double treble) const noexcept
{
    const double l = toneStackPotFraction(bass, components_.bassAudio, true);
    const double m = toneStackPotFraction(mid, components_.midAudio, true);
    const double t = toneStackPotFraction(treble, components_.trebleAudio, false);
    return {std::log(l), std::log(m), std::log(t + 0.01), std::log(1.01 - t)};
}

// Halves the chunk until no log-resistance moves by more than kMaxLogStep across it (the smoothers' end values come
// from the closed-form one-pole law). Bounded: at most log2(interval) halvings.
int ToneStack::limitChunk(int len) const noexcept
{
    const auto g0 = logPots(bass_.current(), mid_.current(), treble_.current());
    auto ahead = [&](const OnePoleSmoother& s, double decay) {
        return s.target() + (static_cast<double>(s.current()) - s.target()) * decay;
    };
    while (len > 1) {
        const double decay = std::exp(-len / (kSmoothingSeconds * designer_.sampleRate()));
        const auto g1 = logPots(ahead(bass_, decay), ahead(mid_, decay), ahead(treble_, decay));
        double step = 0.0;
        for (std::size_t j = 0; j < g0.size(); ++j)
            step = std::max(step, std::abs(g1[j] - g0[j]));
        if (step <= kMaxLogStep)
            break;
        len /= 2;
    }
    return len;
}

template <bool Ramp>
void ToneStack::run(float* io, int n) noexcept
{
    double d = cur_.d, r0 = cur_.r[0], r1 = cur_.r[1], r2 = cur_.r[2], z0 = cur_.z[0], z1 = cur_.z[1], z2 = cur_.z[2];
    double x1 = x1_, w0 = w_[0], w1 = w_[1], w2 = w_[2];
    for (int k = 0; k < n; ++k) {
        if constexpr (Ramp) {
            d += step_.d;
            r0 += step_.r[0];
            r1 += step_.r[1];
            r2 += step_.r[2];
            z0 += step_.z[0];
            z1 += step_.z[1];
            z2 += step_.z[2];
        }
        const double in = io[k];
        w0 = x1 + z0 * w0;
        w1 = x1 + z1 * w1;
        w2 = x1 + z2 * w2;
        x1 = in;
        io[k] = static_cast<float>(d * in + r0 * w0 + r1 * w1 + r2 * w2);
    }
    x1_ = x1;
    w_ = {w0, w1, w2};
}

void ToneStack::process(float* io, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        std::fill_n(io, numSamples, 0.0f);
        return;
    }
    for (int i = 0; i < numSamples;) {
        int len = std::min(numSamples - i, interval_);
        bool ramp = false;
        const bool moving = bass_.isSmoothing() || mid_.isSmoothing() || treble_.isSmoothing();
        if (moving)
            len = limitChunk(len);
        if (dirty_ || moving) {
            for (int k = 0; k < len; ++k) {
                bass_.next();
                mid_.next();
                treble_.next();
            }
            ToneStackCoeffs d;
            if (designer_.design(components_, bass_.current(), mid_.current(), treble_.current(), d)) {
                if (jump_) {
                    cur_ = d;
                } else {
                    const double inv = 1.0 / len;
                    step_.d = (d.d - cur_.d) * inv;
                    for (std::size_t j = 0; j < 3; ++j) {
                        step_.r[j] = (d.r[j] - cur_.r[j]) * inv;
                        step_.z[j] = (d.z[j] - cur_.z[j]) * inv;
                    }
                    target_ = d;
                    ramp = true;
                }
                jump_ = false;
                dirty_ = false;
            } else {
                ++failures_;
                dirty_ = jump_; // retry only while there is no valid design yet
            }
        }
        if (ramp) {
            run<true>(io + i, len);
            cur_ = target_; // land exactly
        } else {
            run<false>(io + i, len);
        }
        i += len;
    }
    // Hygiene: non-finite input poisons the state (clear it); long silence must not leave subnormal doubles.
    double* state[4] = {&x1_, &w_[0], &w_[1], &w_[2]};
    bool finite = true;
    for (double* v : state)
        finite = finite && std::isfinite(*v);
    if (!finite)
        clearState();
    else
        for (double* v : state)
            if (std::abs(*v) < 1e-200)
                *v = 0.0;
}

} // namespace osp::dsp::amp
