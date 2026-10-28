#include "osp/dsp/conv/IrLoader.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>

#include <signalsmith-dsp/fft.h>

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

// Σ w_k·|H_k|² and Σ w_k over the reference band, w_k = 1/f_k (pink), from a zero-padded double FFT with bins of at
// most 1 Hz. Both sums run over the same bins in the same order, so |H_k|² = 1 (a unit impulse) gives num == den.
void pinkBandSums(const float* ir, int length, double rate, double& num, double& den)
{
    num = den = 0.0;
    if (length <= 0 || !(rate > 0.0))
        return;
    std::size_t size = 2;
    while (size < static_cast<std::size_t>(length) || static_cast<double>(size) < rate)
        size *= 2;
    std::vector<double> time(size, 0.0);
    for (int n = 0; n < length; ++n)
        time[static_cast<std::size_t>(n)] = static_cast<double>(ir[n]);
    std::vector<std::complex<double>> spectrum(size / 2);
    signalsmith::fft::RealFFT<double> fft(size);
    fft.fft(time.data(), spectrum.data()); // bin 0 packs DC and Nyquist, both outside the band

    const double binHz = rate / static_cast<double>(size);
    const double hi = std::min(IrLoader::kRefHighHz, 0.45 * rate);
    for (std::size_t k = 1; k < spectrum.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < IrLoader::kRefLowHz)
            continue;
        if (f > hi)
            break;
        const double w = 1.0 / f;
        num += w * std::norm(spectrum[k]);
        den += w;
    }
}

} // namespace

double IrLoader::pinkBandGainDb(const float* ir, int length, double rate)
{
    double num = 0.0, den = 0.0;
    pinkBandSums(ir, length, rate, num, den);
    if (!(num > 0.0) || !(den > 0.0))
        return -std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(num / den);
}

double IrLoader::normalise(float* ir, int length, double rate)
{
    const double gainDb = pinkBandGainDb(ir, length, rate);
    if (!std::isfinite(gainDb)) // empty or silent: nothing to scale
        return 0.0;
    const double correctionDb = std::clamp(-gainDb, -kMaxCorrectionDb, kMaxCorrectionDb);
    if (std::abs(correctionDb) < kDeadbandDb)
        return 0.0;
    const double g = std::pow(10.0, correctionDb / 20.0);
    for (int n = 0; n < length; ++n)
        ir[n] = static_cast<float>(static_cast<double>(ir[n]) * g);
    return correctionDb;
}

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

std::vector<float> IrLoader::condition(const float* ir, int length, double irRate, double processRate, IrLevel level)
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
    if (level == IrLevel::Normalise) // on the final taps at the processing rate, i.e. what the convolver runs
        normalise(out.data(), static_cast<int>(out.size()), processRate);
    return out;
}

} // namespace osp::dsp
