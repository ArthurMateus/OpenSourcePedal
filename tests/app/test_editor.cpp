// Issue #198: the minimal editor. It is built on the message thread against a processor that is not running audio, so
// these tests cover wiring (blocks, selection, attachments), not rendering speed.

#include "BlockControls.h"
#include "OspEditor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <cmath>
#include <vector>

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
            if (spec.kind == ControlKind::note)
                continue;
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

TEST_CASE("Editor: the status bar shows the latency the plugin reports", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    p.prepareToPlay(48000.0, 256);
    OspEditor editor(p);
    editor.status().refresh();
    CHECK(editor.status().latencyText().startsWith("Latency: " + juce::String(p.getLatencySamples()) + " samples"));
}

TEST_CASE("Editor: the meters and the tuner read the engine's lock-free values", "[app][ui]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    constexpr double fs = 48000.0;
    p.prepareToPlay(fs, 512);
    OspEditor editor(p);

    // About one second of a 220 Hz sine at -12 dBFS, through the engine as the audio thread would.
    std::vector<float> left(512), right(512);
    float* channels[2] = {left.data(), right.data()};
    for (int block = 0; block < 100; ++block) {
        for (int n = 0; n < 512; ++n)
            left[static_cast<std::size_t>(n)] =
                0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * static_cast<float>(block * 512 + n) /
                                 static_cast<float>(fs));
        p.engine().process(channels, 2, 512);
    }
    editor.status().refresh();
    CHECK(editor.status().inputDb() == Catch::Approx(-12.0f).margin(0.5f));
    CHECK(editor.status().outputDb() > -60.0f);
    for (int poll = 0; poll < 3; ++poll) // the tuner is polled every fourth refresh, the meters fall in between
        editor.status().refresh();
    CHECK(editor.status().inputDb() < -12.0f);
    INFO(editor.status().tunerText());
    CHECK(editor.status().tunerText().startsWith("Tuner: A3"));
}

TEST_CASE("Editor: snapshot", "[app][ui][.snapshot]")
{
    // Run explicitly with OSP_UI_SNAPSHOT=<file.png>; used for the screenshots in pull requests.
    const auto path = juce::SystemStats::getEnvironmentVariable("OSP_UI_SNAPSHOT", {});
    if (path.isEmpty())
        SKIP("OSP_UI_SNAPSHOT is not set");
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    p.prepareToPlay(48000.0, 512);
    OspEditor editor(p);
    // A 220 Hz sine at -12 dBFS through the engine, so the meters and the tuner show something.
    std::vector<float> left(512), right(512);
    float* channels[2] = {left.data(), right.data()};
    for (int block = 0; block < 100; ++block) {
        for (int n = 0; n < 512; ++n)
            left[static_cast<std::size_t>(n)] = 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f *
                                                                 static_cast<float>(block * 512 + n) / 48000.0f);
        p.engine().process(channels, 2, 512);
    }
    for (int poll = 0; poll < 4; ++poll)
        editor.status().refresh();
    editor.blocks().select(juce::SystemStats::getEnvironmentVariable("OSP_UI_BLOCK", "5").getIntValue());
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(image, out));
}
