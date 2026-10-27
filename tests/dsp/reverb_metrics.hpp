#pragma once

// Test-side reverb measures shared by the FDN and plate tank tests (issue #72): T_mid RT60, tail spectral flatness,
// normalised echo density and the L/R correlation. Float64, test code only.

#include "osp/dsp/util/Svf.h"

#include "measure.hpp"

#include <cmath>
#include <numbers>
#include <vector>

namespace osp::test {

// T30 of one band from the Schroeder energy decay curve of L + R: RT60 = 2·(t(−35 dB) − t(−5 dB)). The band edges
// are two cascaded 2nd-order Butterworth sections each (a 4th-order Linkwitz-Riley-type slope, −6 dB at the edge).
inline double bandT30(const std::vector<float>& l, const std::vector<float>& r, double fs, double loHz, double hiHz)
{
    using osp::dsp::SvfType;
    const auto hp = osp::dsp::svfDesign(SvfType::highPass, fs, loHz, 0.70710678, 0.0);
    const auto lp = osp::dsp::svfDesign(SvfType::lowPass, fs, hiHz, 0.70710678, 0.0);
    auto band = [&](const std::vector<float>& x) {
        osp::dsp::SvfState s[4];
        std::vector<double> e(x.size());
        for (std::size_t n = 0; n < x.size(); ++n) {
            const float v = s[3].tick(lp, s[2].tick(lp, s[1].tick(hp, s[0].tick(hp, x[n]))));
            e[n] = double(v) * v;
        }
        return e;
    };
    const auto el = band(l), er = band(r);
    std::vector<double> edc(l.size() + 1, 0.0);
    for (std::size_t n = l.size(); n-- > 0;)
        edc[n] = edc[n + 1] + el[n] + er[n];
    auto crossing = [&](double db) {
        const double level = edc[0] * std::pow(10.0, db / 10.0);
        std::size_t n = 0;
        while (n < edc.size() && edc[n] > level)
            ++n;
        return static_cast<double>(n) / fs;
    };
    return 2.0 * (crossing(-35.0) - crossing(-5.0));
}

// ISO 3382-1 T_mid: the mean of the 500 Hz and 1 kHz octave-band T30s (bands 354-707 Hz and 707-1414 Hz).
inline double tMid(const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    return 0.5 * (bandT30(l, r, fs, 353.6, 707.1) + bandT30(l, r, fs, 707.1, 1414.2));
}

// Spectral flatness (geometric / arithmetic mean) over [loHz, hiHz] of a Hann-windowed Welch average. `global` uses
// the power spectrum itself, so it also reflects broad coloration. `local` divides by the 1/3-octave smoothed
// envelope first, so it sees only the fine structure: a gross comb (sparse resolved modes) pulls it down.
struct Flatness {
    double global, local;
};
inline Flatness
flatness(const std::vector<float>& x, std::size_t from, std::size_t length, double fs, double loHz, double hiHz)
{
    constexpr std::size_t kFrame = 4096;
    std::vector<double> power(kFrame / 2 + 1, 0.0), frame(kFrame);
    for (std::size_t start = from; start + kFrame <= from + length; start += kFrame / 2) {
        for (std::size_t i = 0; i < kFrame; ++i)
            frame[i] = x[start + i] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / kFrame));
        const auto spec = osp::measure::realFft(frame);
        for (std::size_t k = 0; k < power.size(); ++k)
            power[k] += std::norm(spec[k]);
    }
    double logSum = 0.0, sum = 0.0, localLogSum = 0.0, localSum = 0.0;
    int count = 0;
    for (auto k = static_cast<std::size_t>(loHz * kFrame / fs); k <= static_cast<std::size_t>(hiHz * kFrame / fs);
         ++k) {
        const auto a = static_cast<std::size_t>(double(k) * 0.8909),
                   b = static_cast<std::size_t>(double(k) * 1.1225) + 1;
        double envelope = 0.0;
        for (std::size_t j = a; j <= b; ++j)
            envelope += power[j];
        envelope /= static_cast<double>(b - a + 1);
        logSum += std::log(power[k]);
        sum += power[k];
        localLogSum += std::log(power[k] / envelope);
        localSum += power[k] / envelope;
        ++count;
    }
    return {std::exp(logSum / count) / (sum / count), std::exp(localLogSum / count) / (localSum / count)};
}

// Normalised echo density (J. Abel & P. Huang, "A Simple, Robust Measure of Reverberation Echo Density", AES 121st
// Convention, 2006): over a 20 ms Hann window, the weighted fraction of samples outside one standard deviation,
// divided by erfc(1/√2), the Gaussian value. About 1 once the tail is diffuse. Evaluated every `hop` samples.
struct EchoDensity {
    double mixingMs; // first time the density reaches 0.9 (-1 if never)
    double meanLate; // mean density over [lateFromS, lateToS]
};
inline EchoDensity echoDensity(const std::vector<float>& h, double fs, double lateFromS, double lateToS)
{
    const int width = static_cast<int>(0.020 * fs) | 1, half = width / 2, hop = 48;
    std::vector<double> w(static_cast<std::size_t>(width));
    double wsum = 0.0;
    for (int i = 0; i < width; ++i)
        wsum += w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (i + 1) / (width + 1));
    const double norm = 1.0 / std::erfc(1.0 / std::numbers::sqrt2);
    EchoDensity out{-1.0, 0.0};
    int lateCount = 0;
    for (std::size_t n = static_cast<std::size_t>(half); n + static_cast<std::size_t>(half) < h.size(); n += hop) {
        double s2 = 0.0;
        for (int i = 0; i < width; ++i) {
            const double v = h[n - static_cast<std::size_t>(half) + static_cast<std::size_t>(i)];
            s2 += w[static_cast<std::size_t>(i)] * v * v;
        }
        const double sigma = std::sqrt(s2 / wsum);
        double c = 0.0;
        for (int i = 0; i < width; ++i)
            if (std::abs(h[n - static_cast<std::size_t>(half) + static_cast<std::size_t>(i)]) > sigma)
                c += w[static_cast<std::size_t>(i)];
        const double eta = norm * c / wsum, t = static_cast<double>(n) / fs;
        if (out.mixingMs < 0.0 && sigma > 0.0 && eta >= 0.9)
            out.mixingMs = 1e3 * t;
        if (t >= lateFromS && t < lateToS) {
            out.meanLate += eta;
            ++lateCount;
        }
    }
    out.meanLate /= lateCount > 0 ? lateCount : 1;
    return out;
}

inline double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = from; i < a.size(); ++i) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return ab / std::sqrt(aa * bb);
}

} // namespace osp::test
