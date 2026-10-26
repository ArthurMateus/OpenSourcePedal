#include "spectral.hpp"

#include "measure.hpp"

#include <signalsmith-dsp/fft.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace osp::measure {
namespace {

using Complex = std::complex<double>;

// Signalsmith's packed layout: N/2 bins, with Nyquist in the imaginary part of bin 0. Unnormalised both ways.
std::vector<Complex> packedFft(signalsmith::fft::RealFFT<double>& fft, const std::vector<double>& x)
{
    std::vector<Complex> X(x.size() / 2);
    fft.fft(x.data(), X.data());
    return X;
}

double sweepRate(const EssConfig& c)
{
    // L, rounded so that f1·L is an integer (Novak et al. 2015, eq. 49).
    return std::round(c.f1 * c.seconds / std::log(c.f2 / c.f1)) / c.f1;
}

std::size_t sweepLength(const EssConfig& c)
{
    return static_cast<std::size_t>(std::ceil(sweepRate(c) * std::log(c.f2 / c.f1) * c.sampleRate));
}

} // namespace

std::vector<float> makeEss(const EssConfig& c)
{
    const double L = sweepRate(c);
    const std::size_t n = sweepLength(c);
    std::vector<float> x(n + c.tailSamples, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / c.sampleRate;
        x[i] = static_cast<float>(c.amplitude * std::sin(2.0 * std::numbers::pi * c.f1 * L * std::exp(t / L)));
    }
    return x;
}

EssResponse analyseEss(std::span<const float> output, const EssConfig& c, int maxOrder)
{
    if (maxOrder < 1)
        throw std::invalid_argument("analyseEss: maxOrder must be >= 1");
    const auto sweep = makeEss(c);
    if (output.size() < sweep.size())
        throw std::invalid_argument("analyseEss: output is shorter than makeEss(config)");
    const std::size_t p = std::bit_ceil(2 * output.size());
    signalsmith::fft::RealFFT<double> fft(p);

    std::vector<double> buffer(p, 0.0);
    std::copy(sweep.begin(), sweep.end(), buffer.begin());
    const auto X = packedFft(fft, buffer);
    std::fill(buffer.begin(), buffer.end(), 0.0);
    std::copy(output.begin(), output.end(), buffer.begin());
    const auto Y = packedFft(fft, buffer);

    // Regularised deconvolution: H = Y·X* / (|X|² + ε). Bin 0 holds DC and Nyquist as two real values.
    double peak = 0.0;
    for (const auto& v : X)
        peak = std::max({peak, v.real() * v.real(), v.imag() * v.imag(), std::norm(v)});
    const double eps = 1e-8 * peak;
    std::vector<Complex> H(X.size());
    H[0] = {Y[0].real() * X[0].real() / (X[0].real() * X[0].real() + eps),
            Y[0].imag() * X[0].imag() / (X[0].imag() * X[0].imag() + eps)};
    for (std::size_t b = 1; b < X.size(); ++b)
        H[b] = Y[b] * std::conj(X[b]) / (std::norm(X[b]) + eps);
    std::vector<double> h(p);
    fft.ifft(H.data(), h.data());
    for (auto& v : h)
        v /= static_cast<double>(p);

    // Window each harmonic IR. Order m sits at −L·ln(m); the next order is L·ln((m+1)/m) earlier and the previous
    // one L·ln(m/(m−1)) later, so the window (1/8 before, 7/8 after the peak) fits between them.
    const double L = sweepRate(c);
    const double tightest = maxOrder > 1 ? std::log(double(maxOrder) / double(maxOrder - 1)) : std::log(2.0);
    const std::size_t harmonicWindow = std::min<std::size_t>(8192, std::bit_floor(static_cast<std::size_t>(L * tightest * c.sampleRate)));
    constexpr std::size_t kZeroPad = 16384;
    signalsmith::fft::RealFFT<double> segmentFft(kZeroPad);

    EssResponse r;
    r.binHz_ = c.sampleRate / static_cast<double>(kZeroPad);
    for (int m = 1; m <= maxOrder; ++m) {
        const std::size_t length = m == 1 ? std::min(kZeroPad, std::bit_floor(static_cast<std::size_t>(L * std::log(2.0) * c.sampleRate)))
                                          : harmonicWindow;
        const std::size_t pre = length / 8;
        const auto delay = static_cast<std::size_t>(std::llround(L * std::log(double(m)) * c.sampleRate));
        const std::size_t start = (p - delay - pre) % p;
        std::vector<double> segment(kZeroPad, 0.0);
        for (std::size_t i = 0; i < length; ++i) {
            // Half-Hann fades over the first `pre` and the last `pre` samples.
            const double edge = static_cast<double>(std::min(i, length - 1 - i)) / static_cast<double>(pre);
            const double w = edge >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(std::numbers::pi * edge);
            segment[i] = w * h[(start + i) % p];
        }
        const auto S = packedFft(segmentFft, segment);
        std::vector<double> mag(kZeroPad / 2 + 1);
        mag[0] = std::abs(S[0].real());
        mag[kZeroPad / 2] = std::abs(S[0].imag());
        for (std::size_t b = 1; b < S.size(); ++b)
            mag[b] = std::abs(S[b]);
        r.magnitude_.push_back(std::move(mag));
    }
    return r;
}

double EssResponse::gainDb(int order, double excitationHz) const
{
    if (order < 1 || order > maxOrder())
        throw std::invalid_argument("EssResponse::gainDb: order out of range");
    const auto& mag = magnitude_[static_cast<std::size_t>(order - 1)];
    const double pos = static_cast<double>(order) * excitationHz / binHz_;
    const auto b = static_cast<std::size_t>(pos);
    if (excitationHz < 0.0 || b + 1 >= mag.size())
        throw std::invalid_argument("EssResponse::gainDb: order * f must be below fs/2");
    const double frac = pos - static_cast<double>(b);
    return toDb(mag[b] + frac * (mag[b + 1] - mag[b]));
}

namespace {

constexpr double kFloor = 1e-10; // −100 dBFS (ADR-0007 §3)

// Power spectrogram [frame][bin], normalised so a full-scale sine reads 1 in its bin.
std::vector<std::vector<double>> spectrogram(std::span<const float> x, std::size_t n)
{
    signalsmith::fft::RealFFT<double> fft(n);
    std::vector<double> window(n), frame(n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        sum += window[i];
    }
    const double norm = 1.0 / (0.25 * sum * sum);
    std::vector<std::vector<double>> frames;
    for (std::size_t start = 0; start + n <= x.size(); start += n / 4) {
        for (std::size_t i = 0; i < n; ++i)
            frame[i] = window[i] * static_cast<double>(x[start + i]);
        const auto X = packedFft(fft, frame);
        std::vector<double> power(n / 2 + 1);
        power[0] = X[0].real() * X[0].real() * norm;
        power[n / 2] = X[0].imag() * X[0].imag() * norm;
        for (std::size_t b = 1; b < n / 2; ++b)
            power[b] = std::norm(X[b]) * norm;
        frames.push_back(std::move(power));
    }
    return frames;
}

} // namespace

SpectralDistance spectralDistance(std::span<const float> test, std::span<const float> reference, double sampleRate)
{
    const std::size_t length = std::min(test.size(), reference.size());
    if (length < 2048)
        throw std::invalid_argument("spectralDistance: need at least 2048 common samples");
    test = test.first(length);
    reference = reference.first(length);
    SpectralDistance d;
    int sizes = 0;

    for (std::size_t n : {512u, 1024u, 2048u}) {
        const auto T = spectrogram(test, n), R = spectrogram(reference, n);
        if (R.empty())
            continue;
        double diff = 0.0, ref = 0.0;
        for (std::size_t f = 0; f < R.size(); ++f)
            for (std::size_t b = 0; b < R[f].size(); ++b) {
                const double e = std::sqrt(R[f][b]) - std::sqrt(T[f][b]);
                diff += e * e;
                ref += R[f][b];
            }
        d.spectralConvergence += ref > 0.0 ? std::sqrt(diff / ref) : (diff > 0.0 ? 1.0 : 0.0);
        ++sizes;
        if (n != 2048)
            continue;

        // Mean LSD over frames.
        for (std::size_t f = 0; f < R.size(); ++f) {
            double sq = 0.0;
            for (std::size_t b = 0; b < R[f].size(); ++b) {
                const double e = powerToDb(std::max(T[f][b], kFloor)) - powerToDb(std::max(R[f][b], kFloor));
                sq += e * e;
            }
            d.meanLsdDb += std::sqrt(sq / static_cast<double>(R[f].size())) / static_cast<double>(R.size());
        }

        // 1/3-octave bands centred on 1000·2^(i/3) Hz, 25 Hz … 20 kHz, edges at ×2^(±1/6).
        const double binHz = sampleRate / static_cast<double>(n);
        for (int i = -16; i <= 13; ++i) {
            const double centre = 1000.0 * std::pow(2.0, i / 3.0);
            const auto lo = static_cast<std::size_t>(std::ceil(centre * std::pow(2.0, -1.0 / 6.0) / binHz));
            const auto hi = std::min(n / 2, static_cast<std::size_t>(std::floor(centre * std::pow(2.0, 1.0 / 6.0) / binHz)));
            if (lo > hi)
                continue; // band narrower than a bin
            double sq = 0.0;
            for (std::size_t f = 0; f < R.size(); ++f) {
                double t = 0.0, r = 0.0;
                for (std::size_t b = lo; b <= hi; ++b) {
                    t += T[f][b];
                    r += R[f][b];
                }
                const double e = powerToDb(std::max(t, kFloor)) - powerToDb(std::max(r, kFloor));
                sq += e * e;
            }
            d.maxThirdOctaveLsdDb = std::max(d.maxThirdOctaveLsdDb, std::sqrt(sq / static_cast<double>(R.size())));
        }
    }
    d.spectralConvergence /= sizes; // every size has frames, because length >= 2048
    return d;
}

} // namespace osp::measure
