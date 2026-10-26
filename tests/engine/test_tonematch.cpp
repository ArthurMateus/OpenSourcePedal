// Tone match (ADR-0008). The core check is a round trip: render the bundled CC0 DI through a known setting, treat
// that as the reference, and match from the same DI. The matcher has to find a setting that sounds like it
// (spectral residual and flatness), not necessarily the same knob values, since several settings can sound alike.

#include "osp/dsp/conv/IrLoader.h"
#include "osp/engine/ToneMatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <dr_wav.h>

#include <cmath>
#include <string>
#include <vector>

namespace tmatch = osp::engine::tonematch;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> readMono(const std::string& path, unsigned& rate)
{
    unsigned ch = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
    REQUIRE(raw != nullptr);
    std::vector<float> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i)
        x[i] = raw[i * ch];
    drwav_free(raw, nullptr);
    return x;
}

std::vector<tmatch::CabIr> loadCabs(double fs, std::initializer_list<const char*> names)
{
    std::vector<tmatch::CabIr> cabs;
    for (const char* n : names) {
        unsigned rate = 0;
        const auto raw = readMono(std::string(OSP_ASSET_DIR) + "/cab/" + n + ".wav", rate);
        cabs.push_back({n, std::string("factory:ir/") + n,
                        osp::dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), rate, fs)});
    }
    return cabs;
}

} // namespace

TEST_CASE("tonematch: round trip recovers a hidden drive + cab + EQ setting", "[tonematch][slow]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const auto cabs = loadCabs(rate, {"112-a", "412-a", "412-c", "412-f"});

    struct Case {
        const char* name;
        tmatch::DriveKind kind;
        float knob;
        int cab;
        std::array<float, 9> eq;
    };
    const Case cases[] = {
        {"crunch", tmatch::DriveKind::mid, 6.0f, 2, {2, 0, -3, -2, 0, 3, 2, 0, 0}},
        {"clean", tmatch::DriveKind::clean, 0.0f, 0, {0, 2, 0, 0, -2, 0, 3, 0, 0}},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name)
        {
            tmatch::Setting hidden;
            hidden.inputGainDb = -3.0f; // a different input level than the matcher's -12 dBFS normalisation
            hidden.drive = c.kind;
            hidden.driveKnob = c.knob;
            hidden.cab = c.cab;
            hidden.eqDb = c.eq;
            const auto target = tmatch::analyse(tmatch::render(di, rate, hidden, cabs), rate);
            const auto r = tmatch::match(target, di, rate, cabs);
            WARN(c.name << ": matched " << tmatch::toString(r.setting.drive) << " " << r.setting.driveKnob << ", cab "
                        << cabs[static_cast<std::size_t>(r.setting.cab)].name << ", residual " << r.residualDb
                        << " dB (unmatched " << r.unmatchedResidualDb << " dB), flatness distance "
                        << r.flatnessDistance << " dB");
            CHECK(r.residualDb < 1.0);
            CHECK(r.residualDb < 0.5 * r.unmatchedResidualDb);
            CHECK(r.flatnessDistance < 0.75);
            CHECK((r.setting.drive == tmatch::DriveKind::clean) == (c.kind == tmatch::DriveKind::clean));

            const std::string json =
                tmatch::toPresetJson(r, cabs, "Round trip", "3b0e6a2c-8f41-4d7a-9c1e-5a2b7d9e0f13");
            CHECK(json.find("\"format\": \"osp-preset\"") != std::string::npos);
            CHECK(json.find("\"g1kDb\"") != std::string::npos);
        }
    }
}
