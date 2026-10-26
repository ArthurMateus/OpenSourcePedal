// Self-tests: the measurement harness must report known values on synthetic signals (ADR-0007 §2).

#include "measure.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace osp::measure;
using Catch::Approx;

namespace {

constexpr std::size_t kN = kCoherentFftSize;
constexpr std::size_t kLength = kWarmupSamples + kN;

// y = x + a·x² + c·x³, computed in double and stored as float (like a block's output).
std::vector<float> polynomial(const std::vector<float>& x, double a, double c)
{
    std::vector<float> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double v = x[n];
        y[n] = static_cast<float>(v + a * v * v + c * v * v * v);
    }
    return y;
}

} // namespace

TEST_CASE("Measure: real FFT scaling, DC and Nyquist", "[measure]")
{
    constexpr std::size_t n = 1024;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = 0.25 + 0.5 * std::sin(2.0 * 3.141592653589793 * 37.0 * static_cast<double>(i) / n)
               + 0.125 * (i % 2 == 0 ? 1.0 : -1.0);
    const auto X = realFft(x);
    REQUIRE(X.size() == n / 2 + 1);
    CHECK(std::abs(X[0]) == Approx(0.25 * n));
    CHECK(std::abs(X[37]) == Approx(0.5 * n / 2));
    CHECK(std::abs(X[n / 2]) == Approx(0.125 * n));
    CHECK(std::abs(X[36]) < 1e-9 * n);
}

TEST_CASE("Measure: coherent tone bins are odd and exact", "[measure]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const double f0 = GENERATE(110.0, 440.0, 1000.0, 2500.0, 5000.0, 8000.0);
    const auto tone = coherentTone(f0, fs);
    CHECK(tone.bin % 2 == 1);
    CHECK(std::abs(tone.frequency - f0) <= 2.0 * fs / kN);
    CHECK(tone.frequency == static_cast<double>(tone.bin) * fs / kN);

    CHECK(coherentTone(110.0, 48000.0).bin == 151); // round(150.19) = 150 is even, so 151
    CHECK(coherentTone(1000.0, 48000.0).bin == 1365);
}

TEST_CASE("Measure: THD, ASR and alias-to-fundamental of a known polynomial", "[measure]")
{
    // x = A·sin: x² gives a 2nd harmonic of a·A²/2; x³ gives a 3rd of c·A³/4 and adds 3c·A³/4 to the fundamental.
    constexpr double A = 0.5, a = 0.2, c = 0.5;
    constexpr double fund = A + 3.0 * c * A * A * A / 4.0;
    constexpr double h2 = a * A * A / 2.0;
    constexpr double h3 = c * A * A * A / 4.0;

    SECTION("1 kHz at 48 kHz: no aliasing, THD from H2 and H3")
    {
        const auto tone = coherentTone(1000.0, 48000.0);
        const auto y = polynomial(makeTone(tone, 48000.0, toDb(A), kLength), a, c);
        const auto r = analyseCoherent(y, tone.bin);
        CHECK(r.thd == Approx(std::hypot(h2, h3) / fund).epsilon(1e-5));
        CHECK(r.fundamentalDbfs == Approx(toDb(fund)).margin(1e-4));
        CHECK(r.asrDb < -100.0); // only float32 rounding lands in A
        CHECK_FALSE(r.harmonicsBelowFloor);
    }
    SECTION("10 kHz at 48 kHz: H3 folds to bin N − 3k and counts as alias")
    {
        const auto tone = coherentTone(10000.0, 48000.0);
        REQUIRE(2 * tone.bin < kN / 2);
        REQUIRE(3 * tone.bin > kN / 2);
        const auto y = polynomial(makeTone(tone, 48000.0, toDb(A), kLength), a, c);
        const auto r = analyseCoherent(y, tone.bin);
        CHECK(r.thd == Approx(h2 / fund).epsilon(1e-5));
        CHECK(r.asrDb == Approx(toDb(h3 / h2)).margin(0.01));
        CHECK(r.aliasToFundamentalDb == Approx(toDb(h3 / fund)).margin(0.01));
        CHECK(r.worstAliasDbc == Approx(toDb(h3 / fund)).margin(0.01));
        CHECK(r.thdN == Approx(std::hypot(h2, h3) / fund).epsilon(1e-5));
    }
    SECTION("f0 > fs/4 with an odd polynomial: H is empty, gate on alias-to-fundamental")
    {
        const auto tone = coherentTone(14000.0, 48000.0);
        const auto y = polynomial(makeTone(tone, 48000.0, toDb(A), kLength), 0.0, c);
        const auto r = analyseCoherent(y, tone.bin);
        CHECK(r.harmonicsBelowFloor);
        CHECK(r.aliasToFundamentalDb == Approx(toDb(h3 / fund)).margin(0.01));
    }
}

TEST_CASE("Measure: a linear filter passes the -80 dB alias sanity gate and has the known gain", "[measure]")
{
    // y[n] = (x[n] + x[n−1]) / 2 has |H(f)| = cos(π·f/fs).
    const double fs = 44100.0;
    const auto tone = coherentTone(5000.0, fs);
    const auto x = makeTone(tone, fs, -6.0, kLength);
    std::vector<float> y(x.size());
    for (std::size_t n = 1; n < x.size(); ++n)
        y[n] = 0.5f * (x[n] + x[n - 1]);

    const auto r = analyseCoherent(y, tone.bin);
    CHECK(r.aliasToFundamentalDb <= -80.0);
    CHECK(r.fundamentalDbfs == Approx(-6.0 + toDb(std::cos(3.141592653589793 * tone.frequency / fs))).margin(1e-3));
}

TEST_CASE("Measure: splatter and residual rank a steady tone, smooth ramps and a gain step", "[measure]")
{
    // Gain 1 → 2 (+6 dB) starting mid-record, as REQ-TEST-003 does over 50 ms.
    const double fs = 48000.0;
    const auto tone = coherentTone(1000.0, fs);
    const auto x = makeTone(tone, fs, -6.0, kN);
    const auto rampLength = static_cast<std::size_t>(0.05 * fs);

    enum class Shape { step, linear, raisedCosine };
    auto withGain = [&](Shape shape) {
        std::vector<float> y(x.size());
        const std::size_t start = kN / 2 + 37; // not on a zero crossing
        for (std::size_t n = 0; n < x.size(); ++n) {
            const double t = n < start ? 0.0 : std::min(1.0, double(n - start) / double(rampLength));
            double g = 1.0 + t;
            if (shape == Shape::step)
                g = n < start ? 1.0 : 2.0;
            else if (shape == Shape::raisedCosine)
                g = 1.5 - 0.5 * std::cos(3.141592653589793 * t);
            y[n] = static_cast<float>(0.5 * g * x[n]);
        }
        return y;
    };

    const double steady = splatterDbc(x, fs, tone.frequency);
    const double linear = splatterDbc(withGain(Shape::linear), fs, tone.frequency);
    const double smooth = splatterDbc(withGain(Shape::raisedCosine), fs, tone.frequency);
    const double step = splatterDbc(withGain(Shape::step), fs, tone.frequency);
    CAPTURE(steady, linear, smooth, step);
    CHECK(steady < -100.0);
    CHECK(smooth <= -60.0); // a raised-cosine 50 ms ramp passes REQ-TEST-003 ...
    CHECK(step > -60.0);    // ... a zipper step fails it ...
    CHECK(linear > smooth); // ... and the corners of a linear ramp splatter more (about −56 dBc here)
    CHECK(linear < step - 10.0);

    // Residual variant of REQ-TEST-003: compare against the ideally smoothed offline reference.
    const auto reference = withGain(Shape::raisedCosine);
    CHECK(residualDbc(withGain(Shape::raisedCosine), reference) == -std::numeric_limits<double>::infinity());
    CHECK(residualDbc(withGain(Shape::step), reference) > -60.0);
}

TEST_CASE("Measure: latency-aligned null", "[measure]")
{
    const auto x = makeTone(coherentTone(440.0, 48000.0), 48000.0, -6.0, 4096);
    std::vector<float> delayed(x.size() + 7, 0.0f);
    std::copy(x.begin(), x.end(), delayed.begin() + 7);

    const auto exact = nullTest(delayed, x, 7);
    CHECK(exact.compared == x.size());
    CHECK(std::isinf(exact.rmsDbfs));
    CHECK(exact.rmsDbfs < 0.0);

    CHECK(nullTest(delayed, x, 6).rmsDbfs > -40.0); // misaligned by one sample

    const std::vector<float> silence(64, 0.0f), offset(64, 1e-6f);
    const auto small = nullTest(offset, silence);
    CHECK(small.rmsDbfs == Approx(-120.0).margin(0.1));
    CHECK(small.peakDbfs == Approx(-120.0).margin(0.1));
}
