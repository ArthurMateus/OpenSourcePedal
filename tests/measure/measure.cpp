#include "measure.hpp"

#include <signalsmith-dsp/fft.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

namespace osp::measure {

double toDb(double ratio) noexcept
{
    return ratio > 0.0 ? 20.0 * std::log10(ratio) : -std::numeric_limits<double>::infinity();
}

double powerToDb(double power) noexcept
{
    return power > 0.0 ? 10.0 * std::log10(power) : -std::numeric_limits<double>::infinity();
}

std::vector<std::complex<double>> realFft(std::span<const double> x)
{
    const std::size_t n = x.size();
    assert(n >= 2 && std::has_single_bit(n));
    signalsmith::fft::RealFFT<double> fft(n);
    std::vector<std::complex<double>> half(n / 2);
    fft.fft(x.data(), half.data());

    // Signalsmith packs Nyquist into the imaginary part of bin 0.
    std::vector<std::complex<double>> out(n / 2 + 1);
    out[0] = {half[0].real(), 0.0};
    out[n / 2] = {half[0].imag(), 0.0};
    std::copy(half.begin() + 1, half.end(), out.begin() + 1);
    return out;
}

std::vector<double> powerSpectrum(std::span<const double> x)
{
    const auto spectrum = realFft(x);
    std::vector<double> power(spectrum.size());
    std::transform(spectrum.begin(), spectrum.end(), power.begin(), [](auto c) { return std::norm(c); });
    return power;
}

namespace {

std::vector<double> toDouble(std::span<const float> x)
{
    return {x.begin(), x.end()};
}

} // namespace

CoherentTone coherentTone(double nominalHz, double sampleRate, std::size_t fftSize)
{
    auto k = static_cast<std::size_t>(std::llround(nominalHz * static_cast<double>(fftSize) / sampleRate));
    if (k % 2 == 0)
        ++k;
    return {k, static_cast<double>(k) * sampleRate / static_cast<double>(fftSize)};
}

std::vector<float> makeTone(const CoherentTone& tone, double sampleRate, double amplitudeDbfs, std::size_t length)
{
    const double amplitude = std::pow(10.0, amplitudeDbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * tone.frequency / sampleRate;
    std::vector<float> x(length);
    for (std::size_t n = 0; n < length; ++n)
        x[n] = static_cast<float>(amplitude * std::sin(w * static_cast<double>(n)));
    return x;
}

HarmonicReport analyseCoherent(std::span<const float> output, std::size_t bin, std::size_t fftSize)
{
    assert(output.size() >= fftSize && bin > 0 && bin < fftSize / 2);
    const auto power = powerSpectrum(toDouble(output.last(fftSize)));
    const std::size_t nyquist = fftSize / 2;

    std::vector<bool> isHarmonic(nyquist, false);
    for (std::size_t b = 2 * bin; b < nyquist; b += bin)
        isHarmonic[b] = true;

    const double fund = power[bin];
    double sumH = 0.0, sumA = 0.0, worstA = 0.0;
    for (std::size_t b = 1; b < nyquist; ++b) {
        if (b == bin)
            continue;
        if (isHarmonic[b]) {
            sumH += power[b];
        } else {
            sumA += power[b];
            worstA = std::max(worstA, power[b]);
        }
    }

    // |X[k]| = A·N/2 for a sine of amplitude A, so full-scale power is (N/2)².
    const double fullScale = 0.25 * static_cast<double>(fftSize) * static_cast<double>(fftSize);
    HarmonicReport r;
    r.fundamentalDbfs = powerToDb(fund / fullScale);
    r.thd = std::sqrt(sumH / fund);
    r.thdN = std::sqrt((sumH + sumA) / fund);
    r.asrDb = powerToDb(sumA) - powerToDb(sumH);
    r.aliasToFundamentalDb = powerToDb(sumA / fund);
    r.worstAliasDbc = powerToDb(worstA / fund);
    r.harmonicsBelowFloor = 2 * bin >= nyquist || powerToDb(sumH / fullScale) < -150.0;
    return r;
}

double splatterDbc(std::span<const float> output, double sampleRate, double f0, double guardHz)
{
    // Periodic Hann window: a level change makes the record non-periodic, and a rectangular window would count
    // that wrap-around step as splatter. Hann leakage from the tone is < −100 dBc beyond ±50 Hz at N ≥ 16384.
    auto x = toDouble(output);
    const double n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] *= 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / n);
    const auto power = powerSpectrum(x);
    const double binHz = sampleRate / static_cast<double>(output.size());
    double inside = 0.0, outside = 0.0;
    for (std::size_t b = 0; b < power.size(); ++b) {
        if (std::abs(static_cast<double>(b) * binHz - f0) <= guardHz)
            inside += power[b];
        else
            outside += power[b];
    }
    return powerToDb(outside) - powerToDb(inside);
}

NullResult nullTest(std::span<const float> output, std::span<const float> reference, std::size_t latency)
{
    NullResult r;
    if (output.size() > latency)
        r.compared = std::min(output.size() - latency, reference.size());
    double sumSq = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < r.compared; ++i) {
        const double d = static_cast<double>(output[i + latency]) - static_cast<double>(reference[i]);
        sumSq += d * d;
        peak = std::max(peak, std::abs(d));
    }
    r.rmsDbfs = r.compared > 0 ? toDb(std::sqrt(sumSq / static_cast<double>(r.compared)))
                               : std::numeric_limits<double>::quiet_NaN();
    r.peakDbfs = toDb(peak);
    return r;
}

namespace {

template <typename Ref>
double residualDbcImpl(std::span<const float> output, std::span<const Ref> reference, std::size_t latency)
{
    std::size_t compared = 0;
    if (output.size() > latency)
        compared = std::min(output.size() - latency, reference.size());
    double residual = 0.0, signal = 0.0;
    for (std::size_t i = 0; i < compared; ++i) {
        const double ref = static_cast<double>(reference[i]);
        const double d = static_cast<double>(output[i + latency]) - ref;
        residual += d * d;
        signal += ref * ref;
    }
    return powerToDb(residual) - powerToDb(signal);
}

} // namespace

double residualDbc(std::span<const float> output, std::span<const double> reference, std::size_t latency)
{
    return residualDbcImpl(output, reference, latency);
}

double residualDbc(std::span<const float> output, std::span<const float> reference, std::size_t latency)
{
    return residualDbcImpl(output, reference, latency);
}

} // namespace osp::measure
