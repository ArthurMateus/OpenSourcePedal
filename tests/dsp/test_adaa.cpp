// Adaa1 / Adaa2 (ADR-0005 D1, REQ-OS-004) plus an aliasing check with the ADR-0007 harness.

#include "osp/dsp/oversampling/Adaa.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace osp::dsp;
using Catch::Approx;

namespace {

// f(x) = x: the closed-form kernels are (x0 + x1)/2 and (x0 + x1 + x2)/3 (oversampling.md §2.1).
struct LinearShape {
    static double f(double x) noexcept { return x; }
    static double F1(double x) noexcept { return 0.5 * x * x; }
    static double F2(double x) noexcept { return x * x * x / 6.0; }
};

std::vector<float> randomSignal(std::size_t n, float scale, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-scale, scale);
    std::vector<float> x(n);
    for (auto& v : x)
        v = dist(rng);
    return x;
}

template <typename Adaa>
std::vector<float> run(const std::vector<float>& x)
{
    Adaa adaa;
    std::vector<float> y(x.size());
    adaa.process(x.data(), y.data(), static_cast<int>(x.size()));
    return y;
}

bool allFinite(const std::vector<float>& y)
{
    for (float v : y)
        if (!std::isfinite(v))
            return false;
    return true;
}

} // namespace

TEST_CASE("ADAA: linear case matches the closed-form kernels (REQ-OS-004)", "[dsp][adaa]")
{
    const auto x = randomSignal(4096, 1.0f, 63);
    const auto y1 = run<Adaa1<LinearShape>>(x);
    const auto y2 = run<Adaa2<LinearShape>>(x);
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double x1 = n >= 1 ? x[n - 1] : 0.0, x2 = n >= 2 ? x[n - 2] : 0.0;
        REQUIRE(y1[n] == Approx(0.5 * (x[n] + x1)).margin(1e-6));
        REQUIRE(y2[n] == Approx((x[n] + x1 + x2) / 3.0).margin(1e-5));
    }
    STATIC_REQUIRE(Adaa1<LinearShape>::kDelaySamples == 0.5);
    STATIC_REQUIRE(Adaa2<LinearShape>::kDelaySamples == 1.0);
}

TEST_CASE("ADAA: antiderivatives are consistent (F1' = f, F2' = F1)", "[dsp][adaa]")
{
    constexpr double h = 1e-6;
    for (double x : {-30.0, -2.5, -1.0, -0.999, -0.3, 0.0, 0.4, 1.0, 1.001, 3.0, 30.0}) {
        CAPTURE(x);
        CHECK((TanhShape::F1(x + h) - TanhShape::F1(x - h)) / (2 * h) == Approx(TanhShape::f(x)).margin(1e-6));
        CHECK((HardClipShape::F1(x + h) - HardClipShape::F1(x - h)) / (2 * h) ==
              Approx(HardClipShape::f(x)).margin(1e-6));
        CHECK((HardClipShape::F2(x + h) - HardClipShape::F2(x - h)) / (2 * h) ==
              Approx(HardClipShape::F1(x)).margin(1e-6));
    }
    CHECK(std::isfinite(TanhShape::F1(1e6)));
    CHECK(TanhShape::F1(1e6) == Approx(1e6 - std::log(2.0)));
}

TEMPLATE_TEST_CASE("ADAA: finite and close to f for a slowly varying input (REQ-OS-004)",
                   "[dsp][adaa]",
                   TanhShape,
                   HardClipShape)
{
    // Δx = 5e-8 per sample: every step takes the ill-conditioning fallback.
    std::vector<float> x(4096);
    for (std::size_t n = 0; n < x.size(); ++n)
        x[n] = 0.3f + 5e-8f * static_cast<float>(n);
    const auto y = run<Adaa1<TestType>>(x);
    REQUIRE(allFinite(y));
    for (std::size_t n = 2; n < x.size(); ++n)
        REQUIRE(y[n] == Approx(TestType::f(x[n])).margin(1e-5));

    if constexpr (requires { TestType::F2(0.0); }) {
        const auto y2 = run<Adaa2<TestType>>(x);
        REQUIRE(allFinite(y2));
        for (std::size_t n = 3; n < x.size(); ++n)
            REQUIRE(y2[n] == Approx(TestType::f(x[n])).margin(1e-5));
    }
}

TEMPLATE_TEST_CASE("ADAA: finite for +-1e6 input (REQ-OS-004)", "[dsp][adaa]", TanhShape, HardClipShape)
{
    std::vector<float> x = randomSignal(2048, 1e6f, 7);
    for (std::size_t n = 0; n < 256; ++n)
        x[n] = n % 2 == 0 ? 1e6f : -1e6f; // full-scale alternation
    for (std::size_t n = 256; n < 512; ++n)
        x[n] = 1e6f; // constant: the fallback must give f(1e6)

    const auto y = run<Adaa1<TestType>>(x);
    REQUIRE(allFinite(y));
    CHECK(y[511] == Approx(1.0));
    if constexpr (requires { TestType::F2(0.0); }) {
        const auto y2 = run<Adaa2<TestType>>(x);
        REQUIRE(allFinite(y2));
        CHECK(y2[511] == Approx(1.0));
    }
}

TEST_CASE("ADAA: lowers aliasing of a driven tone at the base rate", "[dsp][adaa]")
{
    // A tone at 48 kHz, driven 4× into the shaper (+12 dB), no oversampling: the effect of ADAA alone.
    using namespace osp::measure;
    const double fs = 48000.0;
    const double f0 = GENERATE(1000.0, 2500.0, 5000.0, 8000.0);
    const auto tone = coherentTone(f0, fs);
    auto x = makeTone(tone, fs, 0.0, kWarmupSamples + kCoherentFftSize);
    for (auto& v : x)
        v *= 4.0f;

    auto naive = [&](auto shape) {
        std::vector<float> y(x.size());
        for (std::size_t n = 0; n < x.size(); ++n)
            y[n] = static_cast<float>(decltype(shape)::f(x[n]));
        return analyseCoherent(y, tone.bin).aliasToFundamentalDb;
    };
    const double tanhNaive = naive(TanhShape{});
    const double tanhAdaa1 = analyseCoherent(run<Adaa1<TanhShape>>(x), tone.bin).aliasToFundamentalDb;
    const double clipNaive = naive(HardClipShape{});
    const double clipAdaa1 = analyseCoherent(run<Adaa1<HardClipShape>>(x), tone.bin).aliasToFundamentalDb;
    const double clipAdaa2 = analyseCoherent(run<Adaa2<HardClipShape>>(x), tone.bin).aliasToFundamentalDb;
    CAPTURE(f0, tanhNaive, tanhAdaa1, clipNaive, clipAdaa1, clipAdaa2);
    // Measured at 48 kHz (MSVC 14.44, 2026-10-26): ADAA1 is 4.6–11 dB below naive at every f0, and ADAA2 a further
    // 6–8 dB below ADAA1. Oversampling (#63 part 2) does the rest of the work toward ADR-0005 D5.
    CHECK(tanhAdaa1 < tanhNaive - 4.0);
    CHECK(clipAdaa1 < clipNaive - 4.0);
    CHECK(clipAdaa2 < clipAdaa1 - 4.0);
}

TEST_CASE("ADAA: RT-clean, and reset() gives bit-identical renders", "[dsp][adaa][rt]")
{
    const auto x = randomSignal(4096, 3.0f, 11);
    std::vector<float> a(x.size()), b(x.size());
    Adaa1<TanhShape> adaa1;
    Adaa2<HardClipShape> adaa2;

    const auto rt = osp::rt::test::runAsAudioThread([&] {
        adaa1.process(x.data(), a.data(), static_cast<int>(x.size()));
        adaa2.process(a.data(), a.data(), static_cast<int>(x.size())); // in place
        adaa1.reset();
        adaa2.reset();
        adaa1.process(x.data(), b.data(), static_cast<int>(x.size()));
        adaa2.process(b.data(), b.data(), static_cast<int>(x.size()));
        adaa1.process(x.data(), b.data(), 0);
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(a);
    REQUIRE(a == b);
}

TEST_CASE("ADAA: Adaa2::reset(x) primes the history, so a constant x outputs exactly f(x)", "[dsp][adaa]")
{
    for (double x : {-3.0, -0.4, 0.0, 0.25, 0.9, 7.0}) {
        CAPTURE(x);
        Adaa2<HardClipShape> a;
        a.reset(x);
        for (int n = 0; n < 4; ++n)
            CHECK(a.processSample(static_cast<float>(x)) ==
                  static_cast<float>(HardClipShape::f(static_cast<float>(x))));
    }
}
