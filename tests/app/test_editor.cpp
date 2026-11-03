// Issue #198: the minimal editor. It is built on the message thread against a processor that is not running audio, so
// these tests cover wiring (blocks, selection, attachments), not rendering speed.

#include "BlockControls.h"
#include "OspEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

namespace {

using namespace osp::ui;

float hostValue(osp::app::PluginProcessor& p, const char* id)
{
    return p.parameters().getRawParameterValue(id)->load();
}

} // namespace

TEST_CASE("Editor: the block row shows every block in signal order and starts on the amp", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    auto* editor = dynamic_cast<OspEditor*>(p.createEditor());
    REQUIRE(editor != nullptr);
    const std::unique_ptr<OspEditor> owner(editor);

    CHECK(editor->blocks().getNumChildComponents() == static_cast<int>(kBlocks.size()));
    CHECK(editor->selectedBlock() == kAmpIndex);
    CHECK(editor->getWidth() * OspEditor::kHeight == editor->getHeight() * OspEditor::kWidth);
}

TEST_CASE("Editor: a block's power LED follows <id>.enabled", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p);

    for (std::size_t i = 0; i < kBlocks.size(); ++i) {
        auto& power = editor.blocks().tile(static_cast<int>(i)).power();
        CHECK(power.isVisible() == kBlocks[i].hasEnabled);
        if (!kBlocks[i].hasEnabled)
            continue;
        const auto id = juce::String(kBlocks[i].id) + ".enabled";
        const bool before = hostValue(p, id.toRawUTF8()) > 0.5f;
        CHECK(power.getToggleState() == before);
        // Host to LED: a parameter change reaches the button on the message thread, which this test is.
        auto* parameter = p.parameters().getParameter(id);
        parameter->setValueNotifyingHost(before ? 0.0f : 1.0f);
        CHECK(power.getToggleState() == !before);
    }
}

TEST_CASE("Editor: clicking a tile selects its block", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p);

    int notified = -2;
    editor.blocks().onSelect = [&](int index) { notified = index; };
    editor.blocks().select(2);
    CHECK(editor.selectedBlock() == 2);
    CHECK(notified == 2);
}

TEST_CASE("Editor: every control of every block maps to an existing host parameter", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    for (std::size_t block = 0; block < kBlocks.size(); ++block) {
        for (const auto& spec : controlsFor(static_cast<int>(block))) {
            for (int channel = 1; channel <= 3; ++channel) {
                const auto id = juce::String(kBlocks[block].id) + "." +
                                juce::String(spec.key).replace("{ch}", juce::String(channel));
                auto* parameter = p.parameters().getParameter(id);
                INFO(id);
                REQUIRE(parameter != nullptr);
                const bool isBool = dynamic_cast<juce::AudioParameterBool*>(parameter) != nullptr;
                const bool isChoice = dynamic_cast<juce::AudioParameterChoice*>(parameter) != nullptr;
                CHECK(isBool == (spec.kind == ControlKind::toggle));
                CHECK(isChoice == (spec.kind == ControlKind::choice));
            }
        }
    }
}

TEST_CASE("Editor: the page shows the controls of the selected block and follows the amp channel", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p);

    for (std::size_t block = 0; block < kBlocks.size(); ++block) {
        editor.blocks().select(static_cast<int>(block));
        CHECK(editor.page().controlCount() == static_cast<int>(controlsFor(static_cast<int>(block)).size()));
    }

    // The cab controls are bound to the active channel: moving the knob of channel 2 changes cab.ch2 only.
    editor.blocks().select(6);
    auto* channel = p.parameters().getParameter("amp.channel");
    channel->setValueNotifyingHost(channel->convertTo0to1(1.0f));
    CHECK(editor.page().controlCount() == static_cast<int>(controlsFor(6).size()));
}

TEST_CASE("Editor: snapshot", "[app][ui][.snapshot]")
{
    // Run explicitly with OSP_UI_SNAPSHOT=<file.png>; used for the screenshots in pull requests.
    const auto path = juce::SystemStats::getEnvironmentVariable("OSP_UI_SNAPSHOT", {});
    if (path.isEmpty())
        SKIP("OSP_UI_SNAPSHOT is not set");
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p);
    editor.blocks().select(juce::SystemStats::getEnvironmentVariable("OSP_UI_BLOCK", "5").getIntValue());
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
}
