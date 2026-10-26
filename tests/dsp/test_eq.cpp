#include "osp/dsp/Eq.h"

#include "measure.hpp"
#include "rt_checker.hpp"
#include "svf_reference.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

using osp::dsp::Eq;
using osp::dsp::EqEdgeShape;
using osp::dsp::EqMode;
using osp::dsp::SvfType;

namespace {

constexpr std::size_t kIrLength = 65536;

// Renders mono `in` in blocks of `blockSize` under the RT checker (block size 0 adds a no-op call before each).
std::vector<float> render(Eq& eq, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out = in;
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            float* ch[1] = {out.data() + pos};
            if (blockSize == 0)
                eq.process(ch, 1, 0);
            eq.process(ch, 1, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

// Magnitude response in dB at `hz`, from the FFT of the impulse response (bins are fs / 65536 apart).
struct Response {
    std::vector<std::complex<double>> spectrum;
    double fs;
    double db(double hz) const
    {
        const auto bin = static_cast<std::size_t>(std::lround(hz * kIrLength / fs));
        return osp::measure::toDb(std::abs(spectrum[bin]));
    }
    double binHz(double hz) const { return std::lround(hz * kIrLength / fs) * fs / kIrLength; }
};

Response measureResponse(Eq& eq, double fs)
{
    std::vector<float> impulse(kIrLength, 0.0f);
    impulse[0] = 1.0f;
    const auto ir = render(eq, impulse, 512);
    return {osp::measure::realFft(std::vector<double>(ir.begin(), ir.end())), fs};
}

std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

} // namespace

TEST_CASE("Eq: defaults are an exact identity (cuts off at their range ends)", "[dsp][eq]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    Eq eq;
    eq.prepare(fs, 2048);
    const auto in = noise(9000, 5u);
    REQUIRE(render(eq, in, 64) == in);
    for (EqMode mode : {EqMode::graphic5, EqMode::parametric}) {
        eq.setMode(mode);
        eq.reset();
        REQUIRE(render(eq, in, 127) == in);
    }
}

TEST_CASE("Eq: graphic bands hit their gain at the centre and match the design curve", "[dsp][eq]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const bool nine = GENERATE(true, false);
    CAPTURE(fs, nine);
    const auto& centres = nine ? std::vector<double>(Eq::kGraphic9Hz.begin(), Eq::kGraphic9Hz.end())
                               : std::vector<double>(Eq::kGraphic5Hz.begin(), Eq::kGraphic5Hz.end());
    const double q = nine ? Eq::kGraphic9Q : Eq::kGraphic5Q;

    // One band alone at +-12 dB: exactly its gain at its centre.
    for (std::size_t b = 0; b < centres.size(); ++b)
        for (float gain : {-12.0f, 12.0f}) {
            Eq eq;
            eq.setMode(nine ? EqMode::graphic9 : EqMode::graphic5);
            nine ? eq.setGraphic9Db(static_cast<int>(b), gain) : eq.setGraphic5Db(static_cast<int>(b), gain);
            eq.prepare(fs, 512);
            const auto r = measureResponse(eq, fs);
            const double f = r.binHz(centres[b]);
            CAPTURE(b, gain, f);
            REQUIRE(std::abs(r.db(f) - osp::test::svfDesignDb(SvfType::bell, fs, centres[b], q, gain, f)) <= 0.01);
            REQUIRE(std::abs(r.db(f) - gain) <= 0.05); // the bin is within fs/65536 of the centre
        }

    // A mixed setting: the cascade at every band centre within +-0.2 dB of the product of the design curves.
    const float gains[9] = {6.0f, -4.0f, 9.0f, -12.0f, 3.0f, 12.0f, -7.0f, 5.0f, -9.0f};
    Eq eq;
    eq.setMode(nine ? EqMode::graphic9 : EqMode::graphic5);
    for (std::size_t b = 0; b < centres.size(); ++b)
        nine ? eq.setGraphic9Db(static_cast<int>(b), gains[b]) : eq.setGraphic5Db(static_cast<int>(b), gains[b]);
    eq.prepare(fs, 512);
    const auto r = measureResponse(eq, fs);
    for (double centre : centres) {
        const double f = r.binHz(centre);
        double design = 0.0;
        for (std::size_t b = 0; b < centres.size(); ++b)
            design += osp::test::svfDesignDb(SvfType::bell, fs, centres[b], q, gains[b], f);
        CAPTURE(centre, r.db(f), design);
        REQUIRE(std::abs(r.db(f) - design) <= 0.2);
    }
}

TEST_CASE("Eq: parametric bells and shelves, edge shapes", "[dsp][eq]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    CAPTURE(fs);
    Eq eq;
    eq.setMode(EqMode::parametric);
    eq.setParametric(0, 120.0f, 6.0f, 0.7f);    // p1 low shelf
    eq.setParametric(1, 700.0f, -9.0f, 2.0f);   // p2 bell
    eq.setParametric(2, 3000.0f, 4.0f, 0.5f);   // p3 bell
    eq.setParametric(3, 8000.0f, -6.0f, 0.7f);  // p4 high shelf
    eq.prepare(fs, 512);
    auto r = measureResponse(eq, fs);
    for (double f : {30.0, 120.0, 700.0, 3000.0, 8000.0, 15000.0}) {
        const double fb = r.binHz(f);
        const double design = osp::test::svfDesignDb(SvfType::lowShelf, fs, 120.0, 0.7, 6.0, fb) +
                              osp::test::svfDesignDb(SvfType::bell, fs, 700.0, 2.0, -9.0, fb) +
                              osp::test::svfDesignDb(SvfType::bell, fs, 3000.0, 0.5, 4.0, fb) +
                              osp::test::svfDesignDb(SvfType::highShelf, fs, 8000.0, 0.7, -6.0, fb);
        CAPTURE(f, r.db(fb), design);
        REQUIRE(std::abs(r.db(fb) - design) <= 0.2);
    }
    // Shelf asymptotes: +6 dB well below p1, -6 dB well above p4 (p2/p3 contribute little there).
    REQUIRE(std::abs(r.db(25.0) - 6.0) <= 0.5);
    REQUIRE(std::abs(r.db(std::min(19000.0, 0.45 * fs)) + 6.0) <= 0.6);

    // Edge shapes as bells: the edge bands now peak at their own centres.
    eq.setParametric(1, 700.0f, 0.0f, 2.0f);
    eq.setParametric(2, 3000.0f, 0.0f, 0.5f);
    eq.setEdgeShape(0, EqEdgeShape::peak);
    eq.setEdgeShape(3, EqEdgeShape::peak);
    eq.reset();
    r = measureResponse(eq, fs);
    REQUIRE(std::abs(r.db(r.binHz(120.0)) - osp::test::svfDesignDb(SvfType::bell, fs, 120.0, 0.7, 6.0, r.binHz(120.0)) -
                     osp::test::svfDesignDb(SvfType::bell, fs, 8000.0, 0.7, -6.0, r.binHz(120.0))) <= 0.05);
}

TEST_CASE("Eq: low and high cut are 2nd-order Butterworth", "[dsp][eq]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    CAPTURE(fs);
    Eq eq;
    eq.setLowCutHz(100.0f);
    eq.setHighCutHz(5000.0f);
    eq.prepare(fs, 512);
    const auto r = measureResponse(eq, fs);
    for (double f : {25.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0}) {
        const double fb = r.binHz(f);
        const double design = osp::test::svfDesignDb(SvfType::highPass, fs, 100.0, Eq::kCutQ, 0.0, fb) +
                              osp::test::svfDesignDb(SvfType::lowPass, fs, 5000.0, Eq::kCutQ, 0.0, fb);
        CAPTURE(f, r.db(fb), design);
        REQUIRE(std::abs(r.db(fb) - design) <= 0.05);
    }
    REQUIRE(std::abs(osp::test::svfDesignDb(SvfType::highPass, fs, 100.0, Eq::kCutQ, 0.0, 100.0) + 3.0103) <= 1e-3);
    REQUIRE(r.db(r.binHz(25.0)) < -23.0); // two octaves below: about -24 dB (12 dB/oct)
}

TEST_CASE("Eq: rate x block-size matrix, stereo, reset() and pre-prepare", "[dsp][eq]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto in = noise(6000, 3u);
    auto configure = [&](Eq& eq) {
        for (int b = 0; b < 9; ++b)
            eq.setGraphic9Db(b, static_cast<float>(b % 3 - 1) * 6.0f);
        eq.setLowCutHz(80.0f);
        eq.setHighCutHz(9000.0f);
        eq.setLevelDb(-3.0f);
        eq.prepare(fs, 2048);
    };
    Eq ref;
    configure(ref);
    const auto expected = render(ref, in, 1);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(fs, block);
        Eq eq;
        configure(eq);
        REQUIRE(render(eq, in, block) == expected);
        eq.reset();
        REQUIRE(render(eq, in, block) == expected);
    }

    // Linked stereo: each channel equals the mono render of the same input.
    const auto other = noise(6000, 9u);
    Eq mono2;
    configure(mono2);
    const auto expected2 = render(mono2, other, 64);
    Eq stereo;
    configure(stereo);
    std::vector<float> l = in, r = other;
    for (int pos = 0; pos < 6000; pos += 100) {
        float* ch[2] = {l.data() + pos, r.data() + pos};
        stereo.process(ch, 2, 100);
    }
    REQUIRE(l == expected);
    REQUIRE(r == expected2);

    Eq unprepared;
    std::vector<float> out(64, 1.0f);
    float* ch[1] = {out.data()};
    unprepared.process(ch, 1, 64);
    REQUIRE(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));

    ref.setBypassed(true);
    REQUIRE(render(ref, in, 64) == in);
}

TEST_CASE("Eq: reversing an edge shape mid-fade does not click (PR #91 review)", "[dsp][eq][xf]")
{
    // p1 low shelf 60 Hz +12 dB Q 1 on a 50 Hz sine at 0.5, 48 kHz / 64. Shelf -> peak at block 188, then back to
    // shelf 512 samples later (g ~ 0.47 of the 20 ms fade). Restarting the still-audible shelf there was a 0.54 step.
    const double fs = 48000.0;
    constexpr int kBlock = 64;
    const int reverseAfter = GENERATE(0, 8); // 0: one plain switch, no reversal (the reference kink)
    CAPTURE(reverseAfter);
    Eq eq;
    eq.setMode(EqMode::parametric);
    eq.setParametric(0, 60.0f, 12.0f, 1.0f);
    eq.prepare(fs, kBlock);

    std::vector<float> y(static_cast<std::size_t>(0.5 * fs));
    for (std::size_t n = 0; n < y.size(); ++n)
        y[n] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 50.0 * static_cast<double>(n) / fs));
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int b = 0; b * kBlock < static_cast<int>(y.size()); ++b) {
            if (b == 188)
                eq.setEdgeShape(0, EqEdgeShape::peak);
            if (reverseAfter > 0 && b == 188 + reverseAfter)
                eq.setEdgeShape(0, EqEdgeShape::shelf);
            float* ch[1] = {y.data() + b * kBlock};
            eq.process(ch, 1, kBlock);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(y);

    double maxD2 = 0.0;
    for (std::size_t n = static_cast<std::size_t>(0.1 * fs); n + 2 < y.size(); ++n)
        maxD2 = std::max(maxD2, std::abs(double(y[n + 2]) - 2.0 * y[n + 1] + y[n]));
    CAPTURE(maxD2);
    REQUIRE(maxD2 < 1e-2); // the review's bound; the plain switch measured 2.1e-3 in the review probe
}
