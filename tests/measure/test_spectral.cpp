// Self-tests: ESS harmonic separation and tier-2 spectral distances on synthetic signals (ADR-0007 §2, §3).

#include "measure.hpp"
#include "spectral.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

using namespace osp::measure;
using Catch::Approx;

TEST_CASE("ESS: a wire has a flat H1 and no harmonics", "[measure]")
{
    const EssConfig config;
    const auto x = makeEss(config);
    const auto r = analyseEss(x, config);
    REQUIRE(r.maxOrder() == 5);
    for (double f : {100.0, 1000.0, 3000.0}) {
        CAPTURE(f);
        CHECK(r.gainDb(1, f) == Approx(0.0).margin(0.05));
        for (int m = 2; m <= 5; ++m)
            CHECK(r.gainDb(m, f) < -80.0);
    }
    CHECK(r.gainDb(1, 10000.0) == Approx(0.0).margin(0.05));
}

TEST_CASE("ESS: H1 of a known filter", "[measure]")
{
    // y[n] = (x[n] + x[n−1]) / 2 has |H(f)| = cos(π·f/fs).
    const EssConfig config;
    const auto x = makeEss(config);
    std::vector<float> y(x.size());
    for (std::size_t n = 1; n < x.size(); ++n)
        y[n] = 0.5f * (x[n] + x[n - 1]);
    const auto r = analyseEss(y, config);
    for (double f : {100.0, 1000.0, 5000.0, 10000.0, 15000.0}) {
        CAPTURE(f);
        CHECK(r.gainDb(1, f) == Approx(toDb(std::cos(3.141592653589793 * f / config.sampleRate))).margin(0.05));
    }
}

TEST_CASE("ESS: H2 and H3 of a known polynomial", "[measure]")
{
    // y = x + a·x² + c·x³ with x = A·sin. Relative to the sweep amplitude: H1 = 1 + 3c·A²/4, H2 = a·A/2,
    // H3 = c·A²/4. f2 = 4 kHz keeps the 5th harmonic below Nyquist, so nothing aliases.
    EssConfig config;
    config.f2 = 4000.0;
    constexpr double A = 0.5, a = 0.2, c = 0.5;
    config.amplitude = A;
    const auto x = makeEss(config);
    std::vector<float> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double v = x[n];
        y[n] = static_cast<float>(v + a * v * v + c * v * v * v);
    }
    const auto r = analyseEss(y, config);
    for (double f : {200.0, 500.0, 1000.0}) {
        CAPTURE(f);
        CHECK(r.gainDb(1, f) == Approx(toDb(1.0 + 3.0 * c * A * A / 4.0)).margin(0.1));
        CHECK(r.gainDb(2, f) == Approx(toDb(a * A / 2.0)).margin(0.2));
        CHECK(r.gainDb(3, f) == Approx(toDb(c * A * A / 4.0)).margin(0.2));
        CHECK(r.gainDb(4, f) < r.gainDb(3, f) - 40.0);
    }
}

TEST_CASE("Spectral distance: LSD, 1/3-octave LSD and MR-STFT convergence", "[measure]")
{
    constexpr double fs = 48000.0;
    std::mt19937 rng(62);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> ref(48000);
    for (auto& v : ref)
        v = noise(rng);

    SECTION("identical renders")
    {
        const auto d = spectralDistance(ref, ref, fs);
        CHECK(d.meanLsdDb == 0.0);
        CHECK(d.maxThirdOctaveLsdDb == 0.0);
        CHECK(d.spectralConvergence == 0.0);
    }
    SECTION("a 1 dB gain change reads 1 dB everywhere and |g − 1| convergence")
    {
        const float g = static_cast<float>(std::pow(10.0, 1.0 / 20.0));
        std::vector<float> test(ref);
        for (auto& v : test)
            v *= g;
        const auto d = spectralDistance(test, ref, fs);
        CHECK(d.meanLsdDb == Approx(1.0).margin(0.01));
        CHECK(d.maxThirdOctaveLsdDb == Approx(1.0).margin(0.01));
        CHECK(d.spectralConvergence == Approx(g - 1.0).epsilon(1e-4));
    }
    SECTION("content below the −100 dBFS floor does not count")
    {
        const std::vector<float> silence(ref.size(), 0.0f);
        std::vector<float> faint(ref);
        for (auto& v : faint)
            v *= 1e-5f; // about −120 dBFS per bin
        const auto d = spectralDistance(faint, silence, fs);
        CHECK(d.meanLsdDb == 0.0);
        CHECK(d.maxThirdOctaveLsdDb == 0.0);
    }
}

TEST_CASE("Spectral and ESS: invalid input fails loudly in every build type", "[measure]")
{
    const std::vector<float> shortClip(2047, 0.1f);
    CHECK_THROWS_AS(spectralDistance(shortClip, shortClip, 48000.0), std::invalid_argument);

    const EssConfig config;
    const auto x = makeEss(config);
    CHECK_THROWS_AS(analyseEss(std::span<const float>(x).first(x.size() - 1), config), std::invalid_argument);
    const auto r = analyseEss(x, config);
    CHECK_THROWS_AS(r.gainDb(6, 1000.0), std::invalid_argument);
    CHECK_THROWS_AS(r.gainDb(3, 9000.0), std::invalid_argument); // 27 kHz is above fs/2
}
