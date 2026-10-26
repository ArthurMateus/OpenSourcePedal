// Tone profile, see ToneProfile.h and ADR-0008. Offline: allocation and exceptions are fine here.

#include "osp/engine/ToneProfile.h"

#include <signalsmith-dsp/fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace osp::engine::tonematch {

namespace {

constexpr int kFftSize = 4096;
constexpr int kHop = 1024;
constexpr int kHpssHalf = 8; // 17-point median filters (FitzGerald 2010 uses 17)
constexpr double kFloor = 1e-20;
constexpr std::array<double, kFlatnessBands + 1> kFlatEdgesHz{500.0, 1000.0, 2000.0, 4000.0, 8000.0};

double medianOf(std::vector<float>& v)
{
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

} // namespace

double bandHz(int band) noexcept
{
    return kBandBaseHz * std::exp2(static_cast<double>(band) / kBandsPerOctave);
}

double fitWeight(int band) noexcept
{
    const double f = bandHz(band);
    if (f >= 120.0 && f <= 6000.0)
        return 1.0;
    if (f >= 80.0 && f <= 10000.0)
        return 0.5;
    return 0.0;
}

const char* toString(Source s) noexcept
{
    switch (s) {
    case Source::mono:
        return "mono";
    case Source::mid:
        return "mid";
    case Source::side:
        return "side";
    case Source::left:
        return "left";
    case Source::right:
        return "right";
    default:
        return "auto";
    }
}

double weightedMean(const Bands& x) noexcept
{
    double s = 0.0, w = 0.0;
    for (int k = 0; k < kBands; ++k) {
        s += fitWeight(k) * x[k];
        w += fitWeight(k);
    }
    return s / w;
}

Bands normalised(Bands x) noexcept
{
    const double m = weightedMean(x);
    for (auto& v : x)
        v -= m;
    return x;
}

double residualDb(const Bands& a, const Bands& b) noexcept
{
    Bands d{};
    for (int k = 0; k < kBands; ++k)
        d[k] = a[k] - b[k];
    const double m = weightedMean(d);
    double s = 0.0, w = 0.0;
    for (int k = 0; k < kBands; ++k) {
        s += fitWeight(k) * (d[k] - m) * (d[k] - m);
        w += fitWeight(k);
    }
    return std::sqrt(s / w);
}

double flatnessDistance(const Profile& a, const Profile& b) noexcept
{
    double s = 0.0;
    for (int i = 0; i < kFlatnessBands; ++i)
        s += (a.flatnessDb[i] - b.flatnessDb[i]) * (a.flatnessDb[i] - b.flatnessDb[i]);
    return std::sqrt(s / kFlatnessBands);
}

std::vector<float>
pickSource(const float* left, const float* right, std::size_t n, double sampleRate, Source wanted, Source* used)
{
    std::vector<float> out(n);
    if (right == nullptr) {
        std::copy(left, left + n, out.begin());
        if (used != nullptr)
            *used = Source::mono;
        return out;
    }
    Source s = wanted;
    if (s == Source::automatic || s == Source::mono) {
        // Energies in the guitar range only (kSideBandHz, two one-pole low passes subtracted): the kick and bass
        // below it would otherwise outweigh the guitars in the mid channel of almost any rock mix.
        const double aLo = std::exp(-2.0 * std::numbers::pi * kSideBandHz[0] / sampleRate);
        const double aHi = std::exp(-2.0 * std::numbers::pi * kSideBandHz[1] / sampleRate);
        double mid = 0.0, side = 0.0, mLo = 0.0, mHi = 0.0, dLo = 0.0, dHi = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double m = 0.5 * (left[i] + right[i]), d = 0.5 * (left[i] - right[i]);
            mLo = m + aLo * (mLo - m);
            mHi = m + aHi * (mHi - m);
            dLo = d + aLo * (dLo - d);
            dHi = d + aHi * (dHi - d);
            mid += (mHi - mLo) * (mHi - mLo);
            side += (dHi - dLo) * (dHi - dLo);
        }
        s = (wanted == Source::automatic && side >= kSideShare * mid) ? Source::side : Source::mid;
    }
    for (std::size_t i = 0; i < n; ++i) {
        switch (s) {
        case Source::side:
            out[i] = 0.5f * (left[i] - right[i]);
            break;
        case Source::left:
            out[i] = left[i];
            break;
        case Source::right:
            out[i] = right[i];
            break;
        default:
            out[i] = 0.5f * (left[i] + right[i]);
            break;
        }
    }
    if (used != nullptr)
        *used = s;
    return out;
}

std::array<double, kBands> bandLevelsDb(const std::vector<double>& power, double binHz)
{
    Bands sum{}, weight{};
    for (std::size_t i = 1; i < power.size(); ++i) {
        const double pos = kBandsPerOctave * std::log2(static_cast<double>(i) * binHz / kBandBaseHz);
        if (pos <= -1.0 || pos >= kBands)
            continue;
        const int lo = static_cast<int>(std::floor(pos));
        const double frac = pos - lo;
        if (lo >= 0) {
            sum[lo] += (1.0 - frac) * power[i];
            weight[lo] += 1.0 - frac;
        }
        if (lo + 1 < kBands) {
            sum[lo + 1] += frac * power[i];
            weight[lo + 1] += frac;
        }
    }
    Bands db{};
    for (int k = 0; k < kBands; ++k) {
        double p = 0.0;
        if (weight[k] > 0.0) {
            p = sum[k] / weight[k];
        } else { // band narrower than a bin: nearest bin
            const auto i = static_cast<std::size_t>(std::lround(bandHz(k) / binHz));
            p = i < power.size() ? power[i] : 0.0;
        }
        db[k] = 10.0 * std::log10(std::max(p, kFloor));
    }
    return db;
}

std::array<double, kBands> impulseBandsDb(const std::vector<float>& ir, double sampleRate)
{
    std::size_t n = 8192;
    while (n < 2 * ir.size())
        n *= 2;
    std::vector<double> x(n, 0.0);
    std::copy(ir.begin(), ir.end(), x.begin());
    std::vector<std::complex<double>> X(n / 2);
    signalsmith::fft::RealFFT<double> fft(n);
    fft.fft(x.data(), X.data());
    std::vector<double> p(n / 2);
    for (std::size_t i = 1; i < n / 2; ++i)
        p[i] = std::norm(X[i]);
    return bandLevelsDb(p, sampleRate / static_cast<double>(n));
}

Profile analyse(const std::vector<float>& x, double fs, const AnalysisOptions& o)
{
    const auto len = std::min(x.size(), static_cast<std::size_t>(o.maxSeconds * fs));
    if (len < static_cast<std::size_t>(kFftSize))
        throw std::invalid_argument("tonematch: signal shorter than one analysis frame");
    const auto frames = (len - kFftSize) / kHop + 1;
    constexpr std::size_t bins = kFftSize / 2;

    // Power spectrogram, periodic Hann.
    std::vector<double> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize);
    signalsmith::fft::RealFFT<double> fft(kFftSize);
    std::vector<double> buf(kFftSize);
    std::vector<std::complex<double>> X(bins);
    std::vector<float> mag(frames * bins);
    for (std::size_t f = 0; f < frames; ++f) {
        for (int i = 0; i < kFftSize; ++i)
            buf[static_cast<std::size_t>(i)] =
                x[f * kHop + static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        fft.fft(buf.data(), X.data());
        X[0] = {X[0].real(), 0.0}; // packed Nyquist out
        for (std::size_t b = 0; b < bins; ++b)
            mag[f * bins + b] = static_cast<float>(std::abs(X[b]));
    }

    // Harmonic power: Wiener mask H² / (H² + P²) from a time median (H) and a frequency median (P).
    std::vector<double> power(frames * bins);
    if (o.hpss) {
        std::vector<float> h(frames * bins), w;
        w.reserve(2 * kHpssHalf + 1);
        for (std::size_t b = 0; b < bins; ++b)
            for (std::size_t f = 0; f < frames; ++f) {
                w.clear();
                for (std::size_t g = f > kHpssHalf ? f - kHpssHalf : 0; g <= std::min(frames - 1, f + kHpssHalf); ++g)
                    w.push_back(mag[g * bins + b]);
                h[f * bins + b] = static_cast<float>(medianOf(w));
            }
        for (std::size_t f = 0; f < frames; ++f)
            for (std::size_t b = 0; b < bins; ++b) {
                w.clear();
                for (std::size_t c = b > kHpssHalf ? b - kHpssHalf : 0; c <= std::min(bins - 1, b + kHpssHalf); ++c)
                    w.push_back(mag[f * bins + c]);
                const double p = medianOf(w), hv = h[f * bins + b], m = mag[f * bins + b];
                const double den = hv * hv + p * p;
                power[f * bins + b] = den > 0.0 ? m * m * (hv * hv / den) : 0.0;
            }
    } else {
        for (std::size_t i = 0; i < power.size(); ++i)
            power[i] = static_cast<double>(mag[i]) * mag[i];
    }

    // Silence gate relative to the loud frames.
    std::vector<double> energy(frames, 0.0);
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t b = 1; b < bins; ++b)
            energy[f] += power[f * bins + b];
    auto sorted = energy;
    std::sort(sorted.begin(), sorted.end());
    const double loud = sorted[std::min(frames - 1, frames * 95 / 100)];
    const double gate = loud * std::pow(10.0, o.gateDb / 10.0);
    std::vector<std::size_t> active;
    for (std::size_t f = 0; f < frames; ++f)
        if (energy[f] > gate && energy[f] > 0.0)
            active.push_back(f);
    if (active.empty())
        throw std::invalid_argument("tonematch: signal is silent");

    std::vector<double> ltas(bins, 0.0);
    for (std::size_t f : active)
        for (std::size_t b = 0; b < bins; ++b)
            ltas[b] += power[f * bins + b];
    for (auto& v : ltas)
        v /= static_cast<double>(active.size());

    Profile prof;
    const double binHz = fs / kFftSize;
    prof.levelDb = normalised(bandLevelsDb(ltas, binHz));
    prof.activeSeconds = static_cast<double>(active.size()) * kHop / fs;

    // Flatness of each frame's spectrum divided by the LTAS: a static linear filter (EQ, cab) multiplies both by the
    // same |H|², so this feature is blind to tone shaping and only sees the harmonic fill that clipping adds.
    for (int band = 0; band < kFlatnessBands; ++band) {
        const auto b0 = static_cast<std::size_t>(std::ceil(kFlatEdgesHz[static_cast<std::size_t>(band)] / binHz));
        const auto b1 =
            std::min(bins - 1, static_cast<std::size_t>(kFlatEdgesHz[static_cast<std::size_t>(band) + 1] / binHz));
        double acc = 0.0;
        for (std::size_t f : active) {
            double logSum = 0.0, linSum = 0.0;
            for (std::size_t b = b0; b <= b1; ++b) {
                const double r = std::max(power[f * bins + b] / std::max(ltas[b], kFloor), 1e-12);
                logSum += std::log(r);
                linSum += r;
            }
            const double cnt = static_cast<double>(b1 - b0 + 1);
            acc += 10.0 * std::log10(std::exp(logSum / cnt) / (linSum / cnt));
        }
        prof.flatnessDb[static_cast<std::size_t>(band)] = acc / static_cast<double>(active.size());
    }
    return prof;
}

} // namespace osp::engine::tonematch
