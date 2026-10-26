#pragma once

// Antiderivative antialiasing (ADAA) for memoryless waveshapers (ADR-0005 D1, oversampling.md §2.1).
// Pure C++20: no JUCE, no OS headers.
//
// Sources:
// - 1st order: J. D. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
//   Continuous-Time Convolution", DAFx-16, 2016: y[n] = (F1(x[n]) − F1(x[n−1])) / (x[n] − x[n−1]).
// - 2nd order: S. Bilbao, F. Esqueda, J. D. Parker, V. Välimäki, "Antiderivative Antialiasing for Memoryless
//   Nonlinearities", IEEE Signal Processing Letters 24(7), 2017:
//   y[n] = 2/(x[n] − x[n−2]) · (D(x[n], x[n−1]) − D(x[n−1], x[n−2])), D(a, b) = (F2(a) − F2(b)) / (a − b).
//   Linear case (f(x) = x): (x[n] + x[n−1] + x[n−2]) / 3, one sample of delay.
// - The x[n] ≈ x[n−2] fallback is the limit of that expression, derived here:
//   y = 2·(D(x1, x̄) − F1(x̄)) / (x1 − x̄) with x̄ = (x[n] + x[n−2]) / 2.
//
// Arithmetic is in double: F2 differences cancel badly in float. Nothing allocates. Every call is noexcept.

#include <cmath>
#include <numbers>

namespace osp::dsp {

// tanh: F1 = log(cosh x) in the overflow-safe form |x| + log1p(e^(−2|x|)) − log 2 (ADR-0005 D1). 1st order only.
struct TanhShape {
    static double f(double x) noexcept { return std::tanh(x); }
    static double F1(double x) noexcept
    {
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
    }
};

// Hard clip to [−1, 1], with closed-form F1 and F2 (continuous with their derivatives at ±1).
struct HardClipShape {
    static double f(double x) noexcept { return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x); }
    static double F1(double x) noexcept
    {
        const double a = std::abs(x);
        return a <= 1.0 ? 0.5 * x * x : a - 0.5;
    }
    static double F2(double x) noexcept
    {
        const double a = std::abs(x);
        const double v = a <= 1.0 ? a * a * a / 6.0 : 0.5 * a * a - 0.5 * a + 1.0 / 6.0;
        return x < 0.0 ? -v : v;
    }
};

// Below this |Δx|, the divided differences are ill-conditioned and the midpoint fallback is used (ADR-0005 D1).
inline constexpr double kAdaaTolerance = 1e-5;

template <typename Shape>
class Adaa1 {
public:
    static constexpr double kDelaySamples = 0.5; // at the rate it runs at (the oversampled rate in a block)

    // RT-safe. Clears the history, so two renders after reset() are bit-identical.
    void reset() noexcept
    {
        x1_ = 0.0;
        f1x1_ = Shape::F1(0.0);
    }

    float processSample(float in) noexcept
    {
        const double x = in;
        const double fx = Shape::F1(x);
        const double dx = x - x1_;
        const double y = std::abs(dx) < kAdaaTolerance ? Shape::f(0.5 * (x + x1_)) : (fx - f1x1_) / dx;
        x1_ = x;
        f1x1_ = fx;
        return static_cast<float>(y);
    }

    // RT-safe. in == out is allowed.
    void process(const float* in, float* out, int numSamples) noexcept
    {
        for (int n = 0; n < numSamples; ++n)
            out[n] = processSample(in[n]);
    }

private:
    double x1_ = 0.0;
    double f1x1_ = Shape::F1(0.0);
};

template <typename Shape>
class Adaa2 {
public:
    static constexpr double kDelaySamples = 1.0;

    void reset() noexcept
    {
        x1_ = x2_ = 0.0;
        f2x1_ = Shape::F2(0.0);
        d12_ = Shape::F1(0.0);
    }

    float processSample(float in) noexcept
    {
        const double x0 = in;
        const double f2x0 = Shape::F2(x0);
        const double d01 = divided(x0, x1_, f2x0, f2x1_);
        double y;
        if (std::abs(x0 - x2_) >= kAdaaTolerance) {
            y = 2.0 * (d01 - d12_) / (x0 - x2_);
        } else {
            const double mid = 0.5 * (x0 + x2_);
            const double dm = x1_ - mid;
            y = std::abs(dm) < kAdaaTolerance
                    ? Shape::f((x0 + x1_ + x2_) / 3.0)
                    : 2.0 * (divided(x1_, mid, f2x1_, Shape::F2(mid)) - Shape::F1(mid)) / dm;
        }
        x2_ = x1_;
        x1_ = x0;
        f2x1_ = f2x0;
        d12_ = d01;
        return static_cast<float>(y);
    }

    void process(const float* in, float* out, int numSamples) noexcept
    {
        for (int n = 0; n < numSamples; ++n)
            out[n] = processSample(in[n]);
    }

private:
    // D(a, b) = (F2(a) − F2(b)) / (a − b), or F1 at the midpoint when a ≈ b.
    static double divided(double a, double b, double f2a, double f2b) noexcept
    {
        const double d = a - b;
        return std::abs(d) < kAdaaTolerance ? Shape::F1(0.5 * (a + b)) : (f2a - f2b) / d;
    }

    double x1_ = 0.0, x2_ = 0.0;
    double f2x1_ = Shape::F2(0.0);
    double d12_ = Shape::F1(0.0);
};

} // namespace osp::dsp
