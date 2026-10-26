#include "osp/dsp/util/Svf.h"

#include "measure.hpp"
#include "rt_checker.hpp"
#include "svf_reference.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <vector>
#include <xmmintrin.h>

using osp::dsp::SvfCoeffs;
using osp::dsp::SvfState;
using osp::dsp::SvfType;

namespace {

constexpr std::size_t kN = 65536;

// FFT of the impulse response of one SVF with fixed coefficients.
std::vector<std::complex<double>> response(const SvfCoeffs& c)
{
    SvfState s;
    std::vector<double> ir(kN);
    for (std::size_t n = 0; n < kN; ++n)
        ir[n] = s.tick(c, n == 0 ? 1.0f : 0.0f);
    return osp::measure::realFft(ir);
}

} // namespace

TEST_CASE("Svf: every type matches the prewarped design curve; bell/shelf/cut anchors", "[dsp][svf]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const SvfType type =
        GENERATE(SvfType::bell, SvfType::lowShelf, SvfType::highShelf, SvfType::highPass, SvfType::lowPass);
    const double f0 = GENERATE(63.0, 1000.0, 12000.0);
    const double q = GENERATE(0.3, 0.7071, 6.0);
    const double gain = GENERATE(-15.0, 6.0);
    CAPTURE(fs, static_cast<int>(type), f0, q, gain);
    const auto h = response(osp::dsp::svfDesign(type, fs, f0, q, gain));
    double worst = 0.0;
    for (std::size_t bin = 4; bin < kN / 2; bin += 97) {
        const double f = static_cast<double>(bin) * fs / kN;
        const double design = osp::test::svfDesignDb(type, fs, f0, q, gain, f);
        if (design < -60.0)
            continue; // deep in a cut's stop band, float coefficient rounding dominates (irrelevant for an EQ)
        worst = std::max(worst, std::abs(osp::measure::toDb(std::abs(h[bin])) - design));
    }
    CAPTURE(worst);
    REQUIRE(worst <= 0.02); // measured worst 0.0096 dB (HPF near its -60 dB edge)

    // Closed-form anchors, independent of the reference: bell centre gain, Butterworth -3 dB.
    if (type == SvfType::bell)
        REQUIRE(std::abs(osp::test::svfDesignDb(type, fs, f0, q, gain, f0) - gain) <= 1e-9);
    if (type == SvfType::highPass || type == SvfType::lowPass)
        if (q == 0.7071)
            REQUIRE(std::abs(osp::test::svfDesignDb(type, fs, f0, q, 0.0, f0) + 3.0103) <= 1e-3);
}

TEST_CASE("Svf: coefficients changing every sample stay bounded, finite and RT-clean", "[dsp][svf]")
{
    // Worst case for a modulated filter: a bell swept 20 Hz <-> 20 kHz, +-15 dB, Q 0.3 <-> 6 every 4800 samples,
    // with new coefficients on every sample, on full-scale noise.
    const double fs = 48000.0;
    std::vector<float> out(480000);
    std::uint32_t seed = 1u;
    SvfState s;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t n = 0; n < out.size(); ++n) {
            const double phase = std::fmod(static_cast<double>(n) / 4800.0, 2.0);
            const double t = phase < 1.0 ? phase : 2.0 - phase;
            const auto c = osp::dsp::svfDesign(SvfType::bell, fs, 20.0 * std::pow(1000.0, t), 0.3 * std::pow(20.0, t),
                                               -15.0 + 30.0 * t);
            seed = seed * 1664525u + 1013904223u;
            out[n] = s.tick(c, static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    float peak = 0.0f;
    for (float v : out)
        peak = std::max(peak, std::abs(v));
    CAPTURE(peak);
    REQUIRE(peak < 40.0f); // +15 dB on a full-scale input is 5.6; a TPT SVF has no modulation blow-up
}

TEST_CASE("Svf: decaying states snap to zero, no subnormals even with FTZ off", "[dsp][svf]")
{
    REQUIRE((_mm_getcsr() & 0x8040u) == 0u); // FTZ and DAZ are off outside the RT harness
    for (SvfType type : {SvfType::bell, SvfType::lowShelf, SvfType::highShelf, SvfType::highPass, SvfType::lowPass}) {
        CAPTURE(static_cast<int>(type));
        const auto c = osp::dsp::svfDesign(type, 48000.0, 1000.0, 0.7, 12.0);
        SvfState s;
        std::vector<float> y(48000);
        for (std::size_t n = 0; n < y.size(); ++n)
            y[n] = s.tick(c, n == 0 ? 1.0f : 0.0f);
        CHECK(osp::rt::test::scanOutput(y).subnormal == 0u);
        CHECK(s.ic1 == 0.0f);
        CHECK(s.ic2 == 0.0f);
    }
}
