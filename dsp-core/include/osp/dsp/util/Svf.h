#pragma once

// Linear trapezoidal (TPT) state-variable filter: bell, shelves, 2nd-order high/low pass. Pure C++20, RT-safe.
//
// A. Simper, "Linear Trapezoidal Integrated State Variable Filter With Low Noise Optimisation" (Cytomic technical
// paper, 2013), and "Solving the continuous SVF equations using trapezoidal integration and equivalent currents"
// (2011). The structure stays stable and click-free when its coefficients change every sample, which is what
// parameters.md §1.4 asks of filter frequency/Q/gain smoothing. g = tan(pi f / fs) prewarps the bilinear transform
// at f, so the centre frequency and centre gain are exact at every sample rate.

#include <cmath>

namespace osp::dsp {

enum class SvfType { bell, lowShelf, highShelf, highPass, lowPass };

struct SvfCoeffs {
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f; // integrator update
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f; // output mix: m0·input + m1·band + m2·low
};

// Non-allocating, noexcept; used per sample while a parameter moves. hz is clamped below 0.49·fs.
inline SvfCoeffs svfDesign(SvfType type, double sampleRate, double hz, double q, double gainDb) noexcept
{
    const double f = hz < 0.49 * sampleRate ? hz : 0.49 * sampleRate;
    double g = std::tan(3.14159265358979323846 * f / sampleRate);
    double k = 1.0 / q;
    const double a = std::pow(10.0, gainDb / 40.0); // sqrt of the linear gain
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
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
    const double a1 = 1.0 / (1.0 + g * (g + k));
    return {static_cast<float>(a1), static_cast<float>(g * a1), static_cast<float>(g * g * a1),
            static_cast<float>(m0), static_cast<float>(m1),     static_cast<float>(m2)};
}

struct SvfState {
    float ic1 = 0.0f, ic2 = 0.0f;

    float tick(const SvfCoeffs& c, float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        // Decaying states snap at -400 dB, so silence never leaves subnormals behind, even with FTZ off.
        if (std::abs(ic1) < 1.0e-20f)
            ic1 = 0.0f;
        if (std::abs(ic2) < 1.0e-20f)
            ic2 = 0.0f;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }
};

} // namespace osp::dsp
