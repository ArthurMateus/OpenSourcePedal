// Issue #199, part 2: the tone-match panel. The match runs on a worker thread; the panel is polled by hand here because
// the tests run no message loop.

#include "OspEditor.h"
#include "PresetStore.h"
#include "ToneMatchPanel.h"

#include "AudioFiles.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace osp::ui;

namespace {

struct Work {
    fs::path dir, di, user;
    Work()
        : dir(fs::temp_directory_path() / ("osp-panel-" + std::to_string(std::rand()))),
          di(dir / "di.wav"),
          user(dir / "user")
    {
        fs::create_directories(dir);
#ifdef NDEBUG
        constexpr double kSeconds = 5.0;
#else
        constexpr double kSeconds = 3.0; // Debug builds are about 20x slower
#endif
        const auto src =
            osp::app::decodeAudioFile(fs::path(OSP_SOURCE_ASSET_DIR) / "di" / "strum-arp-01.wav", kSeconds);
        REQUIRE(src.has_value());
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::FileOutputStream>(juce::File(juce::CharPointer_UTF16(di.c_str())));
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions{}.withSampleRate(src->sampleRate).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        const float* channel = src->left.data();
        REQUIRE(writer->writeFromFloatArrays(&channel, 1, static_cast<int>(src->left.size())));
    }
    ~Work()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

juce::String str(const fs::path& p)
{
    return juce::String(juce::CharPointer_UTF16(p.c_str()));
}

void waitFor(ToneMatchPanel& panel, ToneMatchPanel::Phase leave)
{
    for (int i = 0; i < 6000 && panel.phase() == leave; ++i) {
        panel.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

TEST_CASE("Tone-match panel: a run produces a preset that auditions and saves", "[app][ui][tonematch][slow]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const Work work;
    osp::app::PluginProcessor p;
    p.prepareToPlay(48000.0, 256);
    ToneMatchPanel panel(p, work.user, work.dir / "dl");

    CHECK_FALSE(panel.audition()); // nothing to play yet
    CHECK_FALSE(panel.saveResult());

    panel.setReference(str(work.di)); // the DI as its own "song"
    panel.setDi(str(work.di));
    panel.setPresetName("Panel test");
    REQUIRE(panel.run());
    CHECK(panel.phase() == ToneMatchPanel::Phase::matching);
    CHECK_FALSE(panel.run()); // one run at a time

    waitFor(panel, ToneMatchPanel::Phase::matching);
    REQUIRE(panel.phase() == ToneMatchPanel::Phase::done);
    CHECK(panel.progress() == 1.0);

    REQUIRE(panel.audition());
    CHECK(p.presetName() == "Panel test");

    REQUIRE(panel.saveResult());
    const auto listed = osp::app::listPresets({}, work.user);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "Panel test");
    CHECK(osp::app::loadPresetFile(listed[0].path).has_value()); // "produces a preset that loads"
}

TEST_CASE("Tone-match panel: cancel stops the run and leaves nothing to save", "[app][ui][tonematch][slow]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const Work work;
    osp::app::PluginProcessor p;
    ToneMatchPanel panel(p, work.user, work.dir / "dl");
    panel.setReference(str(work.di));
    panel.setDi(str(work.di));
    REQUIRE(panel.run());
    panel.cancel();
    waitFor(panel, ToneMatchPanel::Phase::matching);
    CHECK(panel.phase() == ToneMatchPanel::Phase::idle);
    CHECK(panel.statusText() == "Cancelled");
    CHECK_FALSE(panel.saveResult());
}

TEST_CASE("Tone-match panel: missing or bad inputs are explained, not run", "[app][ui][tonematch]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const Work work;
    osp::app::PluginProcessor p;
    ToneMatchPanel panel(p, work.user, work.dir / "dl");

    CHECK_FALSE(panel.run());
    CHECK(panel.statusText().contains("Choose the song"));

    panel.setReference(str(work.di));
    panel.setDi(str(work.di));
    panel.setSection("30", "10");
    CHECK_FALSE(panel.run());
    CHECK(panel.statusText().contains("start < end"));
    panel.setSection("1:x", "");
    CHECK_FALSE(panel.run());

    panel.setSection("", "");
    panel.setReference("--exec=calc");
    CHECK(panel.run() == true); // not a link: treated as a file path, which fails below
    waitFor(panel, ToneMatchPanel::Phase::matching);
    CHECK(panel.phase() == ToneMatchPanel::Phase::failed);

    panel.setReference("https://a.com/x y");
    CHECK_FALSE(panel.run());
    CHECK(panel.statusText().contains("not a plain http(s)"));
}

TEST_CASE("Tone-match panel: only the standalone app shows it", "[app][ui][tonematch]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;
    OspEditor editor(p); // the test host is not the standalone wrapper
    CHECK(editor.toneMatch() == nullptr);
    CHECK(editor.getWidth() * OspEditor::kHeight == editor.getHeight() * OspEditor::kWidth);
}
