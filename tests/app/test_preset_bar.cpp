// Issue #199, part 1: the preset menu. The controller does what the menu items do; the bar shows its result.

#include "OspEditor.h"
#include "PresetBar.h"

#include "osp/engine/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace osp::ui;
using namespace osp::engine;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag)
        : path(fs::temp_directory_path() / ("osp-bar-" + tag + "-" + std::to_string(std::rand())))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void setParam(osp::app::PluginProcessor& p, const char* id, float plain)
{
    auto* param = p.parameters().getParameter(id);
    REQUIRE(param != nullptr);
    param->setValueNotifyingHost(param->convertTo0to1(plain));
}

float hostValue(osp::app::PluginProcessor& p, const char* id)
{
    return p.parameters().getRawParameterValue(id)->load();
}

std::size_t indexOf(const PresetController& c, const std::string& name)
{
    for (std::size_t i = 0; i < c.entries().size(); ++i)
        if (c.entries()[i].name == name)
            return i;
    FAIL("preset not listed: " << name);
    return 0;
}

} // namespace

TEST_CASE("Preset menu: save then load gives the same sound back", "[app][ui][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TempDir user("user");
    osp::app::PluginProcessor p;
    PresetController controller(p, {}, user.path);

    setParam(p, "output.levelDb", -7.0f);
    setParam(p, "amp.ch1.gain", 8.0f);
    CHECK(controller.saveAs("  My lead ") == "Saved My lead");
    REQUIRE(controller.entries().size() == 1);
    CHECK(controller.entries()[0].name == "My lead");
    CHECK_FALSE(controller.entries()[0].factory);

    setParam(p, "output.levelDb", 0.0f);
    setParam(p, "amp.ch1.gain", 2.0f);
    CHECK(controller.load(indexOf(controller, "My lead")) == "Loaded My lead");
    CHECK(hostValue(p, "output.levelDb") == -7.0f);
    CHECK(hostValue(p, "amp.ch1.gain") == 8.0f);
    CHECK(p.presetName() == "My lead");
}

TEST_CASE("Preset menu: Save overwrites a user preset and asks for a name otherwise", "[app][ui][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TempDir user("save");
    osp::app::PluginProcessor p;
    PresetController controller(p, {}, user.path);

    CHECK(controller.save().empty()); // an unsaved Init has nothing to overwrite
    controller.saveAs("Rhythm");
    setParam(p, "amp.ch1.gain", 3.0f);
    CHECK(controller.save() == "Saved Rhythm");
    CHECK(controller.entries().size() == 1); // same id: the file was replaced, not duplicated

    setParam(p, "amp.ch1.gain", 9.0f);
    controller.load(0);
    CHECK(hostValue(p, "amp.ch1.gain") == 3.0f);
}

TEST_CASE("Preset menu: blank names, bad files and missing folders are reported, not fatal", "[app][ui][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TempDir user("bad");
    {
        std::ofstream(user.path / "broken.osp.json") << "{ not json";
    }
    osp::app::PluginProcessor p;
    PresetController controller(p, user.path / "no-such-factory-folder", user.path);

    CHECK(controller.entries().empty()); // the broken file is skipped
    CHECK(controller.saveAs("   ") == "Give the preset a name");
    CHECK(controller.load(5) == "No such preset");
    CHECK(PresetController(p, {}, {}).saveAs("x") == "No user preset folder");
}

TEST_CASE("Preset menu: the bar shows the current preset and marks unsaved ones", "[app][ui][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p);
    CHECK(editor.presets().nameText() == "Init");
    p.beginNewPreset("Crunch");
    editor.presets().refresh();
    CHECK(editor.presets().nameText() == "Crunch");
}

TEST_CASE("Preset menu: a changed parameter marks the preset unsaved, loading or saving clears it", "[app][ui][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    TempDir user("dirty");
    osp::app::PluginProcessor p;
    PresetController controller(p, {}, user.path);
    OspEditor editor(p);

    CHECK_FALSE(p.isDirty());
    setParam(p, "amp.ch1.gain", 4.5f);
    CHECK(p.isDirty());
    editor.presets().refresh();
    CHECK(editor.presets().nameText() == "Init *");

    controller.saveAs("Edited");
    CHECK_FALSE(p.isDirty());
    setParam(p, "amp.ch1.gain", 6.0f);
    CHECK(p.isDirty());
    controller.load(0);
    CHECK_FALSE(p.isDirty()); // applying a preset is not an edit
}
