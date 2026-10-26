#include "osp/dsp/conv/IrLoader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace osp::dsp {

namespace {

// Zeroth-order modified Bessel function of the first kind, power series (converges fast for x <= ~20).
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200 && term > 1e-16 * sum; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
    }
    return sum;
}

} // namespace

std::vector<float> IrLoader::resample(const float* ir, int length, double fromRate, double toRate)
{
    if (length <= 0)
        return {};
    if (fromRate == toRate)
        return std::vector<float>(ir, ir + length);

    const double fc = kCutoff * std::min(fromRate, toRate); // Hz
    const double halfWidth = kZeroCrossings / (2.0 * fc);  // seconds
    const double gain = 2.0 * fc / toRate;                  // sinc DC gain x rate ratio
    const double i0Beta = besselI0(kKaiserBeta);
    const auto outLength = static_cast<std::size_t>(std::ceil(static_cast<double>(length) * toRate / fromRate));

    std::vector<float> out(outLength);
    for (std::size_t m = 0; m < outLength; ++m) {
        const double t = static_cast<double>(m) / toRate;
        const auto first = static_cast<int>(std::max(0.0, std::ceil((t - halfWidth) * fromRate)));
        const auto last = static_cast<int>(std::min<double>(length - 1, std::floor((t + halfWidth) * fromRate)));
        double acc = 0.0;
        for (int n = first; n <= last; ++n) {
            const double tau = t - static_cast<double>(n) / fromRate;
            const double u = tau / halfWidth;
            const double x = 2.0 * fc * tau;
            const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double window = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0Beta;
            acc += static_cast<double>(ir[n]) * sinc * window;
        }
        out[m] = static_cast<float>(acc * gain);
    }
    return out;
}

std::vector<float> IrLoader::condition(const float* ir, int length, double irRate, double processRate)
{
    std::vector<float> clean(ir, ir + std::max(length, 0));
    for (auto& v : clean)
        if (!std::isfinite(v))
            v = 0.0f;
    auto out = resample(clean.data(), static_cast<int>(clean.size()), irRate > 0.0 ? irRate : processRate, processRate);

    const auto cap = static_cast<std::size_t>(std::lround(kMaxIrSeconds * processRate));
    if (out.size() > cap) {
        out.resize(cap);
        const auto fade = std::min(cap, static_cast<std::size_t>(std::lround(kCapFadeSeconds * processRate)));
        for (std::size_t i = 0; i < fade; ++i) {
            const double phase = std::numbers::pi * (static_cast<double>(i) + 0.5) / static_cast<double>(fade);
            out[cap - fade + i] *= static_cast<float>(0.5 * (1.0 + std::cos(phase)));
        }
    }
    return out;
}

} // namespace osp::dsp
