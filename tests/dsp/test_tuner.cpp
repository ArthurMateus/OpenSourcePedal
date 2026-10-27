// Chromatic tuner, REQ-419: +-1 cent for 40-1000 Hz sines, stable on plucked notes (#75).

#include "osp/dsp/tuner/PitchDetector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using osp::dsp::PitchDetector;

namespace {

std::vector<float> sine(double hz, double fs, int n, double amp = 0.5, double phase = 0.3)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs + phase));
    return x;
}

// A plucked string without inharmonicity: harmonics 1...12 decaying faster with order, the 2nd louder than the
// fundamental (a classic octave-error trap), plus a little noise.
std::vector<float> pluck(double hz, double fs, int n, unsigned seed = 1)
{
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1e-3f);
    for (int h = 1; h <= 12 && h * hz < 0.45 * fs; ++h) {
        const double amp = (h == 2 ? 0.5 : 0.35) / h, decay = 1.5 * h;
        for (int i = 0; i < n; ++i)
            x[static_cast<std::size_t>(i)] += static_cast<float>(
                amp * std::exp(-decay * i / fs) * std::sin(2.0 * std::numbers::pi * h * hz * i / fs + 0.7 * h));
    }
    for (auto& v : x)
        v += noise(rng);
    return x;
}

double centsError(double got, double want)
{
    return 1200.0 * std::log2(got / want);
}

} // namespace

TEST_CASE("PitchDetector: +-1 cent on 40-1000 Hz sines (REQ-419)", "[dsp][tuner]")
{
    const double fs = GENERATE(22050.0, 24000.0); // the tap's decimated rates (44.1/88.2 and 48/96 kHz)
    PitchDetector det;
    det.prepare(fs);
    double worst = 0.0;
    for (double hz = 40.0; hz <= 1000.0; hz *= std::pow(2.0, 1.0 / 7.0)) { // 7 per octave, off the semitone grid
        const auto x = sine(hz, fs, det.requiredSamples());
        const auto r = det.detect(x.data(), static_cast<int>(x.size()));
        CAPTURE(fs, hz);
        REQUIRE(r.valid);
        const double e = centsError(r.hz, hz);
        worst = std::max(worst, std::abs(e));
        CHECK(std::abs(e) <= 1.0);
    }
    WARN("worst sine error at " << fs << " Hz: " << worst << " cents");
}

TEST_CASE("PitchDetector: plucked notes with a strong 2nd harmonic read at the fundamental", "[dsp][tuner]")
{
    const double fs = 24000.0;
    PitchDetector det;
    det.prepare(fs);
    // Open strings of a standard-tuned guitar, then A4 and a high fret.
    for (double hz : {82.407, 110.0, 146.832, 195.998, 246.942, 329.628, 440.0, 880.0}) {
        const auto x = pluck(hz, fs, det.requiredSamples() + 2000);
        const auto r = det.detect(x.data(), static_cast<int>(x.size()));
        CAPTURE(hz);
        REQUIRE(r.valid);
        CHECK(std::abs(centsError(r.hz, hz)) <= 1.0);
        CHECK(r.confidence > 0.8);
    }
}

TEST_CASE("PitchDetector: note names, cents and the reference pitch", "[dsp][tuner]")
{
    auto r = PitchDetector::toNote(440.0, 440.0);
    CHECK(r.midiNote == 69);
    CHECK_THAT(r.cents, WithinAbs(0.0, 1e-9));
    r = PitchDetector::toNote(82.407, 440.0); // low E, E2 = MIDI 40
    CHECK(r.midiNote == 40);
    CHECK(std::abs(r.cents) < 0.1);
    r = PitchDetector::toNote(440.0 * std::pow(2.0, 0.3 / 12.0), 440.0); // A4 + 30 cents
    CHECK(r.midiNote == 69);
    CHECK_THAT(r.cents, WithinAbs(30.0, 1e-6));
    r = PitchDetector::toNote(432.0, 432.0); // the reference defines A4
    CHECK(r.midiNote == 69);
    CHECK_THAT(r.cents, WithinAbs(0.0, 1e-9));
    CHECK_FALSE(PitchDetector::toNote(0.0, 440.0).valid);

    PitchDetector det;
    det.setReferenceHz(400.0);
    CHECK(det.referenceHz() == 415.0);
    det.setReferenceHz(500.0);
    CHECK(det.referenceHz() == 466.0);
    det.setReferenceHz(std::nan(""));
    CHECK(det.referenceHz() == 440.0);
}

TEST_CASE("PitchDetector: silence, noise, short input and unprepared calls are invalid", "[dsp][tuner]")
{
    PitchDetector det;
    std::vector<float> x(4096, 0.0f);
    CHECK_FALSE(det.detect(x.data(), static_cast<int>(x.size())).valid); // before prepare()
    det.prepare(24000.0);
    CHECK_FALSE(det.detect(x.data(), static_cast<int>(x.size())).valid); // silence
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    for (auto& v : x)
        v = noise(rng);
    CHECK_FALSE(det.detect(x.data(), static_cast<int>(x.size())).valid); // white noise has no period
    const auto s = sine(440.0, 24000.0, det.requiredSamples() - 1);
    CHECK_FALSE(det.detect(s.data(), static_cast<int>(s.size())).valid); // one sample short
    x.assign(x.size(), std::nanf(""));
    CHECK_FALSE(det.detect(x.data(), static_cast<int>(x.size())).valid);
}
