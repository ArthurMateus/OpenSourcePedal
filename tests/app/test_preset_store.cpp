#include "PresetStore.h"

#include "osp/engine/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace osp::app;
using namespace osp::engine;

namespace {

// A fresh folder under the system temp dir, removed on exit.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag)
        : path(fs::temp_directory_path() / ("osp-test-" + tag + "-" + std::to_string(std::rand())))
    {
        fs::remove_all(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Preset sample(const std::string& id, const std::string& name, float outputDb)
{
    std::vector<float> values;
    for (const auto& p : paramTable())
        values.push_back(p.def);
    values[static_cast<std::size_t>(findParam("output.levelDb"))] = outputDb;
    return makePreset(id, name, values, {{0, 0, "factory:ir/412-a", ""}});
}

void write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

} // namespace

TEST_CASE("PresetStore: save then load gives back every value", "[app][presets]")
{
    const TempDir dir("save");
    const auto preset = sample("11111111-2222-4333-8444-555555555555", "My Crunch!", -5.5f);
    std::string error;
    const auto path = saveUserPreset(dir.path / "nested" / "User", preset, &error);
    REQUIRE(path.has_value());
    CHECK(path->filename() == "my-crunch.osp.json");
    const auto back = loadPresetFile(*path, &error);
    REQUIRE(back.has_value());
    CHECK(back->name == "My Crunch!");
    CHECK(presetValues(*back) == presetValues(preset));
    CHECK(back->cabs.size() == 1);
    CHECK_FALSE(fs::exists(fs::path(path->string() + ".tmp")));
}

TEST_CASE("PresetStore: the same id is replaced, another preset with the same name gets a suffix", "[app][presets]")
{
    const TempDir dir("names");
    const auto a = sample("11111111-2222-4333-8444-555555555555", "Lead", -1.0f);
    const auto b = sample("66666666-7777-4888-9999-aaaaaaaaaaaa", "Lead", -2.0f);
    const auto pa = saveUserPreset(dir.path, a);
    const auto pb = saveUserPreset(dir.path, b);
    REQUIRE((pa && pb));
    CHECK(pa->filename() == "lead.osp.json");
    CHECK(pb->filename() == "lead-2.osp.json");

    auto a2 = sample(a.id, "Lead", -3.0f);
    CHECK(saveUserPreset(dir.path, a2) == pa); // overwritten in place
    const auto back = loadPresetFile(*pa);
    REQUIRE(back.has_value());
    CHECK(presetValues(*back)[static_cast<std::size_t>(findParam("output.levelDb"))] == -3.0f);
}

TEST_CASE("PresetStore: listing handles missing folders, bad files and ordering", "[app][presets]")
{
    const TempDir dir("list");
    const auto factory = dir.path / "factory";
    const auto user = dir.path / "user";

    int skipped = -1;
    CHECK(listPresets(factory, user, &skipped).empty()); // neither folder exists yet (the factory set may be absent)
    CHECK(skipped == 0);
    CHECK(listPresets({}, {}).empty());

    auto clean = sample("11111111-2222-4333-8444-555555555555", "Zeta", 0.0f);
    clean.category = "Clean";
    write(factory / "clean" / "zeta.osp.json", writePreset(clean));
    auto alpha = sample("66666666-7777-4888-9999-aaaaaaaaaaaa", "Alpha", 0.0f);
    alpha.category = "Clean";
    write(factory / "clean" / "alpha.osp.json", writePreset(alpha));
    write(factory / "broken.osp.json", "{ not json");
    write(factory / "notes.txt", "ignored");
    REQUIRE(saveUserPreset(user, sample("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff", "Mine", 0.0f)));

    const auto all = listPresets(factory, user, &skipped);
    REQUIRE(all.size() == 3);
    CHECK(skipped == 1);
    CHECK(all[0].name == "Alpha");
    CHECK(all[1].name == "Zeta");
    CHECK(all[0].factory);
    CHECK(all[2].name == "Mine");
    CHECK_FALSE(all[2].factory);
}

TEST_CASE("PresetStore: file names and error paths", "[app][presets]")
{
    CHECK(presetFileSlug("  Hello,   World  ") == "hello-world");
    CHECK(presetFileSlug("!!!") == "preset");
    CHECK(presetFileSlug(std::string(100, 'a')).size() == 48);

    const TempDir dir("errors");
    std::string error;
    CHECK_FALSE(loadPresetFile(dir.path / "missing.osp.json", &error));
    CHECK_FALSE(error.empty());
    write(dir.path / "bad.osp.json", R"({"format":"osp-preset","schemaVersion":9})");
    CHECK_FALSE(loadPresetFile(dir.path / "bad.osp.json", &error));
    CHECK(error.find("schemaVersion") != std::string::npos);
    CHECK_FALSE(saveUserPreset({}, sample("a", "x", 0.0f), &error));
    CHECK(error.find("APPDATA") != std::string::npos);

    CHECK(findFactoryPresetDir({dir.path / "nope", dir.path}) == dir.path);
    CHECK(findFactoryPresetDir({dir.path / "nope"}).empty());
}
