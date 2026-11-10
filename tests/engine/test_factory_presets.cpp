// #200: every factory preset in presets/factory/ is a well-formed preset.v1 document, renders finite and audible at
// 44.1/48/96 kHz, and meets its loudness target on the bundled DI. The full JSON Schema check is
// tools/presets/check_factory.py (CI); the engine has no schema validator, so this test checks the structure it
// relies on and applies every parameter by id (an unknown id fails).

#include "preset_rig.hpp"

#include "osp/dsp/conv/IrLoader.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using osp::test::PresetRig;

namespace {

std::vector<fs::path> factoryFiles()
{
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(OSP_PRESET_DIR "/factory"))
        if (e.is_regular_file() && e.path().string().ends_with(".osp.json"))
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    return files;
}

// The bundled DI at `rate` (band-limited resampling when it differs from the file's 44.1 kHz).
std::vector<float> di(double rate)
{
    unsigned fileRate = 0;
    auto x = osp::test::readMonoWav(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", fileRate);
    if (static_cast<double>(fileRate) != rate)
        x = osp::dsp::IrLoader::resample(x.data(), static_cast<int>(x.size()), fileRate, rate);
    return x;
}

double lufs(const std::vector<float>& l, const std::vector<float>& r, double rate)
{
    std::vector<float> inter(l.size() * 2);
    for (std::size_t i = 0; i < l.size(); ++i) {
        inter[2 * i] = l[i];
        inter[2 * i + 1] = r[i];
    }
    ebur128_state* st = ebur128_init(2, static_cast<unsigned long>(rate), EBUR128_MODE_I);
    REQUIRE(st != nullptr);
    ebur128_add_frames_float(st, inter.data(), l.size());
    double v = -HUGE_VAL;
    ebur128_loudness_global(st, &v);
    ebur128_destroy(&st);
    return v;
}

} // namespace

TEST_CASE("Factory presets: 4 to 6 files, well-formed, original unique names", "[engine][presets]")
{
    const auto files = factoryFiles();
    CHECK(files.size() >= 4);
    CHECK(files.size() <= 6);
    const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    std::set<std::string> ids, names;
    for (const auto& f : files) {
        INFO(f.string());
        const auto p = osp::test::readJson(f);
        CHECK(p.at("format") == "osp-preset");
        CHECK(p.at("schemaVersion") == 1);
        const auto id = p.at("id").get<std::string>();
        CHECK(std::regex_match(id, uuid));
        CHECK(ids.insert(id).second);
        const auto name = p.at("name").get<std::string>();
        CHECK((!name.empty() && name.size() <= 64));
        CHECK(names.insert(name).second);
        CHECK_FALSE(p.value("description", std::string()).empty());
        CHECK_FALSE(p.value("category", std::string()).empty());
        std::set<std::string> blockIds;
        for (const auto& b : p.at("blocks"))
            CHECK(blockIds.insert(b.at("id").get<std::string>()).second);
    }
}

TEST_CASE("Factory presets: finite, audible output at 44.1, 48 and 96 kHz", "[engine][presets]")
{
    const double rate = GENERATE(44100.0, 48000.0, 96000.0);
    const auto input = di(rate);
    for (const auto& f : factoryFiles()) {
        INFO(f.string() << " at " << rate);
        PresetRig rig(osp::test::readJson(f), rate);
        std::vector<float> l, r;
        rig.render(input, l, r);
        double sum = 0.0, peak = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i) {
            REQUIRE(std::isfinite(l[i]));
            REQUIRE(std::isfinite(r[i]));
            sum += static_cast<double>(l[i]) * l[i];
            peak = std::max({peak, std::abs(static_cast<double>(l[i])), std::abs(static_cast<double>(r[i]))});
        }
        const double rmsDb = 10.0 * std::log10(sum / static_cast<double>(l.size()) + 1e-30);
        CHECK(rmsDb > -50.0); // audible
        CHECK(peak < 0.99);   // no clipping
    }
}

// Issue #200: -18 LUFS-I, leads -17 (stereo output, BS.1770-4 gated), within 0.5 LU. Printed so the PR can quote it.
TEST_CASE("Factory presets: loudness target on the bundled DI", "[engine][presets][loudness]")
{
    const auto input = di(44100.0);
    for (const auto& f : factoryFiles()) {
        const auto p = osp::test::readJson(f);
        PresetRig rig(p, 44100.0);
        std::vector<float> l, r;
        rig.render(input, l, r);
        const double target = p.value("category", std::string()) == "Lead" ? -17.0 : -18.0;
        const double got = lufs(l, r, 44100.0);
        std::printf("loudness %-28s %.2f LUFS-I (target %.0f)\n", p.at("name").get<std::string>().c_str(), got, target);
        INFO(f.string());
        CHECK(std::abs(got - target) <= 0.5);
    }
}
