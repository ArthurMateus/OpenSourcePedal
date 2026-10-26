// Tone profile (ADR-0008 D2): band grid, source pick, and the two properties the matcher relies on: the flatness
// feature ignores linear filtering and rises with clipping.

#include "osp/dsp/Eq.h"
#include "osp/engine/ToneProfile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <dr_wav.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace tmatch = osp::engine::tonematch;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> readDi(unsigned& rate)
{
    unsigned ch = 0;
    drwav_uint64 frames = 0;
    const std::string path = std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav";
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
    REQUIRE(raw != nullptr);
    std::vector<float> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i)
        x[i] = raw[i * ch];
    drwav_free(raw, nullptr);
    return x;
}

std::vector<float> sine(double hz, double fs, double seconds, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(fs * seconds));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs));
    return x;
}

} // namespace

TEST_CASE("tonematch: band grid and the band-level mapping of a flat spectrum", "[tonematch]")
{
    CHECK_THAT(tmatch::bandHz(0), WithinAbs(50.0, 1e-9));
    CHECK_THAT(tmatch::bandHz(4), WithinAbs(100.0, 1e-9));
    // A unit impulse is flat: every band reads 0 dB.
    std::vector<float> ir(64, 0.0f);
    ir[0] = 1.0f;
    for (double v : tmatch::impulseBandsDb(ir, 48000.0))
        CHECK_THAT(v, WithinAbs(0.0, 1e-6));
}

TEST_CASE("tonematch: pickSource takes the side channel of a wide mix and the mid of a centred one", "[tonematch]")
{
    const auto centre = sine(110.0, 48000.0, 0.5, 0.2); // a centred "bass"
    const auto wide = sine(440.0, 48000.0, 0.5, 0.6);   // a hard-left "guitar"
    std::vector<float> l(centre.size()), r(centre.size());
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = centre[i] + wide[i];
        r[i] = centre[i];
    }
    tmatch::Source used{};
    const auto side = tmatch::pickSource(l.data(), r.data(), l.size(), 48000.0, tmatch::Source::automatic, &used);
    CHECK(used == tmatch::Source::side);
    for (std::size_t i = 0; i < side.size(); i += 97)
        CHECK_THAT(side[i], WithinAbs(0.5f * wide[i], 1e-6)); // the centred part cancels
    tmatch::pickSource(r.data(), r.data(), r.size(), 48000.0, tmatch::Source::automatic, &used);
    CHECK(used == tmatch::Source::mid);
}

TEST_CASE("tonematch: the flatness feature ignores EQ and rises with clipping", "[tonematch]")
{
    unsigned rate = 0;
    const auto di = readDi(rate);
    const auto clean = tmatch::analyse(di, rate);

    // A 12 dB tilt through the real graphic EQ.
    auto tilted = di;
    osp::dsp::Eq eq;
    eq.prepare(rate, static_cast<int>(tilted.size()));
    const float tilt[9] = {-6, -4, -2, 0, 2, 4, 6, 6, 6};
    for (int b = 0; b < 9; ++b)
        eq.setGraphic9Db(b, tilt[b]);
    eq.reset();
    float* ch[1] = {tilted.data()};
    eq.process(ch, 1, static_cast<int>(tilted.size()));
    const auto t = tmatch::analyse(tilted, rate);
    for (int i = 0; i < tmatch::kFlatnessBands; ++i)
        CHECK_THAT(t.flatnessDb[i], WithinAbs(clean.flatnessDb[i], 0.3)); // measured: <= 0.03 dB per band

    // A hard tanh clipper, 30 dB into it.
    auto driven = di;
    float peak = 0.0f;
    for (float v : di)
        peak = std::max(peak, std::abs(v));
    for (auto& v : driven)
        v = std::tanh(v / peak * 31.6f);
    const auto d = tmatch::analyse(driven, rate);
    double rise = 0.0;
    for (int i = 0; i < tmatch::kFlatnessBands; ++i)
        rise += d.flatnessDb[i] - clean.flatnessDb[i];
    WARN("mean flatness rise with a tanh clipper at +30 dB: " << rise / tmatch::kFlatnessBands << " dB");
    CHECK(rise / tmatch::kFlatnessBands > 1.0);
}
