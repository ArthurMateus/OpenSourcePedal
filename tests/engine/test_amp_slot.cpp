#include "osp/engine/AmpSlot.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

using osp::dsp::OversamplingQuality;
using osp::engine::AmpParams;
using osp::engine::AmpStub;

namespace {

std::vector<float> run(AmpStub& amp, std::vector<float> x, int block)
{
    const int total = static_cast<int>(x.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block)
            amp.process(x.data() + pos, std::min(block, total - pos));
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(x);
    return x;
}

std::vector<float> impulse(std::size_t n)
{
    std::vector<float> x(n, 0.0f);
    x[0] = 1.0f;
    return x;
}

} // namespace

TEST_CASE("AmpStub: identity at the defaults for every block size", "[engine][amp]")
{
    const int block = GENERATE(1, 32, 127, 2048);
    AmpStub amp;
    amp.prepare(48000.0, 2048, OversamplingQuality::live);
    std::vector<float> x(5000);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = std::sin(0.01f * static_cast<float>(i));
    CHECK(run(amp, x, block) == x);
    CHECK(amp.getMeasuredDelaySamples() == 0.0);
}

TEST_CASE("AmpStub: levelDb is the channel level, ramped over 20 ms", "[engine][amp]")
{
    AmpStub amp;
    amp.prepare(48000.0, 64, OversamplingQuality::live);
    AmpParams p;
    p.levelDb = -6.0f;
    amp.setParameters(p);
    const auto y = run(amp, std::vector<float>(2400, 1.0f), 64);
    CHECK(y[0] < 1.0f);
    CHECK(y[0] > 0.99f); // first step of a 960-sample ramp
    CHECK(std::abs(y[2000] - std::pow(10.0f, -0.3f)) < 1e-6f);
    p.levelDb = std::nanf("");
    amp.setParameters(p); // non-finite: the range minimum
    const auto z = run(amp, std::vector<float>(2400, 1.0f), 64);
    CHECK(std::abs(z[2399] - std::pow(10.0f, -1.2f)) < 1e-6f);
}

TEST_CASE("AmpStub: the test delay is a fractional delay with that 1 kHz group delay", "[engine][amp]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    for (const double d : {1.0, 2.37, 3.5}) {
        AmpStub amp(d);
        amp.prepare(fs, 64, OversamplingQuality::live);
        const auto h = run(amp, impulse(256), 64);
        std::complex<double> num, den;
        const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
        for (std::size_t n = 0; n < h.size(); ++n) {
            const auto e = std::polar(static_cast<double>(h[n]), -w * static_cast<double>(n));
            num += static_cast<double>(n) * e;
            den += e;
        }
        CAPTURE(fs, d);
        CHECK(std::abs((num / den).real() - d) < 0.01);
        CHECK(amp.getMeasuredDelaySamples() == d);
    }
}

TEST_CASE("AmpStub: hard bypass is an exact integer delay of the pad the graph assigns", "[engine][amp]")
{
    AmpStub amp(2.37);
    amp.prepare(48000.0, 64, OversamplingQuality::live);
    AmpParams p;
    p.levelDb = -12.0f; // ignored while bypassed
    amp.setParameters(p);
    for (const int pad : {0, 2, 3}) {
        amp.setBypassed(true, pad);
        amp.reset();
        const auto h = run(amp, impulse(64), 16);
        for (std::size_t n = 0; n < h.size(); ++n)
            CHECK(h[n] == (static_cast<int>(n) == pad ? 1.0f : 0.0f));
    }
}
