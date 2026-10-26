#pragma once

// Test-side reference for util/Svf.h: the continuous SVF prototype evaluated at the frequency the prewarped bilinear
// transform maps to, H = m0 + (m1 s + m2) / (s^2 + k s + 1), s = j tan(pi f / fs) / g. Float64, test code only.

#include "osp/dsp/util/Svf.h"

#include <cmath>
#include <complex>

namespace osp::test {

inline double svfDesignDb(osp::dsp::SvfType type, double fs, double f0, double q, double gainDb, double f)
{
    using osp::dsp::SvfType;
    constexpr double kPi = 3.14159265358979323846;
    const double a = std::pow(10.0, gainDb / 40.0);
    double g = std::tan(kPi * f0 / fs), k = 1.0 / q, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    switch (type) {
    case SvfType::bell:
        k = 1.0 / (q * a);
        m1 = k * (a * a - 1.0);
        break;
    case SvfType::lowShelf:
        g /= std::sqrt(a);
        m1 = k * (a - 1.0);
        m2 = a * a - 1.0;
        break;
    case SvfType::highShelf:
        g *= std::sqrt(a);
        m0 = a * a;
        m1 = k * (1.0 - a) * a;
        m2 = 1.0 - a * a;
        break;
    case SvfType::highPass:
        m1 = -k;
        m2 = -1.0;
        break;
    case SvfType::lowPass:
        m0 = 0.0;
        m2 = 1.0;
        break;
    }
    const std::complex<double> s(0.0, std::tan(kPi * f / fs) / g);
    const auto h = m0 + (m1 * s + m2) / (s * s + k * s + 1.0);
    return 20.0 * std::log10(std::abs(h));
}

} // namespace osp::test
