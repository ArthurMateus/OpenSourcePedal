// FractionalDelay and TempoSync (#71 part 1): echo time within +-1 sample (the #71 criterion), interpolation error,
// clamps, pre-prepare, reset, the RT checker, and the sync table.

#include "osp/dsp/delay/TempoSync.h"
#include "osp/dsp/util/FractionalDelay.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

using osp::dsp::FractionalDelay;
using osp::dsp::SyncDivision;
using osp::dsp::TempoSync;

namespace {

// Feeds `in` through a fixed read delay `d` (read, then push) under the RT checker.
std::vector<float> run(FractionalDelay& line, const std::vector<float>& in, float d)
{
    std::vector<float> out(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t n = 0; n < in.size(); ++n) {
            out[n] = line.read(d);
            line.push(in[n]);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

} // namespace

TEST_CASE("FractionalDelay: echo time within +-1 sample at 7 / 400 / 3000 ms, every rate", "[dsp][delay]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double ms = GENERATE(7.0, 123.4567, 400.0, 3000.0);
    CAPTURE(fs, ms);
    const double d = ms * fs / 1000.0;
    FractionalDelay line;
    line.prepare(static_cast<int>(std::ceil(TempoSync::kMaxMs * fs / 1000.0)));
    std::vector<float> in(static_cast<std::size_t>(d) + 64, 0.0f);
    in[0] = 1.0f;
    const auto y = run(line, in, static_cast<float>(d));

    // The echo's peak lands within one sample of the requested time.
    const auto peak = std::distance(y.begin(), std::max_element(y.begin(), y.end()));
    CAPTURE(d, peak);
    REQUIRE(std::abs(static_cast<double>(peak) - d) <= 1.0);
    // And its low-frequency phase delay (100 Hz, from the impulse response) is the requested d to 0.01 sample.
    std::complex<double> h;
    const double w = 2.0 * std::numbers::pi * 100.0 / fs;
    for (std::size_t n = 0; n < y.size(); ++n)
        h += static_cast<double>(y[n]) * std::polar(1.0, -w * static_cast<double>(n));
    double phase = -std::arg(h);
    const double wraps = std::round((w * d - phase) / (2.0 * std::numbers::pi));
    phase += 2.0 * std::numbers::pi * wraps;
    CAPTURE(phase / w);
    REQUIRE(std::abs(phase / w - d) < 0.01);
}

TEST_CASE("FractionalDelay: integer delays are exact; Catmull-Rom error at fractional delays", "[dsp][delay]")
{
    const double fs = 48000.0;
    FractionalDelay line;
    line.prepare(4800);
    std::vector<float> x(8192);
    for (std::size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(0.5 * std::sin(0.37 * static_cast<double>(n)) + 0.3 * std::sin(0.011 * n));
    const auto y = run(line, x, 100.0f);
    for (std::size_t n = 100; n < x.size(); ++n)
        REQUIRE(y[n] == x[n - 100]);

    // Worst case t = 0.5: the error of a sine at 1 / 5 / 10 kHz against the analytic delayed sine.
    for (double f : {1000.0, 5000.0, 10000.0}) {
        line.reset();
        std::vector<float> s(4096);
        for (std::size_t n = 0; n < s.size(); ++n)
            s[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(n) / fs));
        const float d = 37.5f;
        const auto out = run(line, s, d);
        double err = 0.0;
        for (std::size_t n = 64; n < s.size(); ++n)
            err = std::max(err,
                           std::abs(out[n] - std::sin(2.0 * std::numbers::pi * f * (static_cast<double>(n) - d) / fs)));
        const double errDb = 20.0 * std::log10(err);
        CAPTURE(f, errDb);
        WARN("Catmull-Rom error at t = 0.5, " << f << " Hz: " << errDb << " dB");
        REQUIRE(errDb < (f <= 1000.0 ? -90.0 : f <= 5000.0 ? -40.0 : -20.0)); // measured -103 / -48 / -25 dB
    }
}

TEST_CASE("FractionalDelay: clamps, pre-prepare, reset", "[dsp][delay]")
{
    FractionalDelay unprepared;
    REQUIRE(unprepared.read(10.0f) == 0.0f);
    unprepared.push(1.0f); // no-op, no crash

    FractionalDelay line;
    line.prepare(64);
    REQUIRE(line.getMaxDelay() == 64);
    for (int i = 1; i <= 100; ++i)
        line.push(static_cast<float>(i)); // x[n - k] = 101 - k
    REQUIRE(line.read(0.25f) == 100.0f);                                    // below 1 -> 1
    REQUIRE(line.read(std::numeric_limits<float>::quiet_NaN()) == 100.0f); // NaN -> 1
    REQUIRE(line.read(1000.0f) == 101.0f - 64.0f);                          // above max -> max
    REQUIRE(line.read(1.5f) == 99.5f);                                      // linear between 1 and 2
    REQUIRE(line.read(10.5f) == 90.5f); // a ramp is exact for the cubic too
    line.reset();
    REQUIRE(line.read(5.0f) == 0.0f);
}

TEST_CASE("TempoSync: divisions at a tempo, clamped to 7...3000 ms", "[dsp][delay]")
{
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_4, 120.0) == 500.0);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_8d, 120.0) == 375.0);
    REQUIRE(std::abs(TempoSync::timeMs(SyncDivision::n1_4t, 120.0) - 1000.0 / 3.0) < 1e-9);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_1, 120.0) == 2000.0);
    REQUIRE(std::abs(TempoSync::timeMs(SyncDivision::n1_64t, 300.0) - 25.0 / 3.0) < 1e-9);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_64t, 999.0) == TempoSync::kMinMs);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_1d, 40.0) == TempoSync::kMaxMs);
    REQUIRE(TempoSync::timeMs(SyncDivision::off, 120.0) < 0.0);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_4, 0.0) < 0.0);
    REQUIRE(TempoSync::timeMs(SyncDivision::n1_4, std::numeric_limits<double>::quiet_NaN()) < 0.0);
    // Within each note value: triplet < straight < dotted; each straight value doubles the previous one.
    for (int note = 0; note < 7; ++note) {
        const auto t = static_cast<SyncDivision>(1 + 3 * note);
        const auto s = static_cast<SyncDivision>(2 + 3 * note), d = static_cast<SyncDivision>(3 + 3 * note);
        REQUIRE(TempoSync::beats(t) < TempoSync::beats(s));
        REQUIRE(TempoSync::beats(s) < TempoSync::beats(d));
        if (note > 0)
            REQUIRE(TempoSync::beats(s) == 2.0 * TempoSync::beats(static_cast<SyncDivision>(2 + 3 * (note - 1))));
    }
    REQUIRE(TempoSync::beats(SyncDivision::n1_1d) == 6.0);
}
