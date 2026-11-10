#include "PluginProcessor.h"

#include "osp/engine/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace osp::app;
using namespace osp::engine;

namespace {

constexpr const char* kUri = "factory:ir/412-a";

std::vector<float> defaults()
{
    std::vector<float> values;
    for (const auto& p : paramTable())
        values.push_back(p.def);
    return values;
}

void set(std::vector<float>& values, const char* id, float v)
{
    const int index = findParam(id);
    REQUIRE(index >= 0); // an unknown id would write outside the vector
    values[static_cast<std::size_t>(index)] = v;
}

// Mono ramp through a stereo-out processor, left channel; `blocks` of 256 so the audio thread pumps the IR queue.
std::vector<float> render(PluginProcessor& p, int blocks)
{
    p.prepareToPlay(48000.0, 256);
    juce::MidiBuffer midi;
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) {
        juce::AudioBuffer<float> buffer(2, 256);
        for (int n = 0; n < 256; ++n)
            buffer.setSample(0, n, 0.1f * std::sin(0.05f * static_cast<float>(b * 256 + n)));
        p.processBlock(buffer, midi);
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 256);
    }
    return out;
}

} // namespace

TEST_CASE("App: a loaded preset is applied and captured back", "[app][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    PluginProcessor p;
    auto values = defaults();
    set(values, "output.levelDb", -6.0f);
    set(values, "amp.ch1.gain", 7.0f);
    const auto preset = makePreset("11111111-2222-4333-8444-555555555555", "Test tone", values, {{0, 0, kUri, ""}});

    p.prepareToPlay(48000.0, 256);
    const auto warnings = p.loadPreset(preset);
    CHECK(warnings.empty());
    CHECK(p.presetName() == "Test tone");
    CHECK_FALSE(p.isDirty());

    const auto back = p.capturePreset();
    CHECK(presetValues(back) == values);
    REQUIRE(back.cabs.size() == 1);
    CHECK(back.cabs[0].uri == kUri);
}

TEST_CASE("App: the plugin state carries the whole preset", "[app][preset][state]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    auto values = defaults();
    set(values, "amp.ch1.gain", 8.5f);
    set(values, "output.levelDb", -2.5f);
    const auto preset = makePreset("11111111-2222-4333-8444-555555555555", "Kept", values, {{0, 0, kUri, ""}});

    PluginProcessor a;
    a.loadPreset(preset);
    juce::MemoryBlock blob;
    a.getStateInformation(blob);

    PluginProcessor b;
    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    CHECK(presetValues(b.capturePreset()) == values);
    CHECK(b.presetName() == "Kept");
    CHECK(b.capturePreset().cabs.size() == 1);

    juce::MemoryBlock again;
    b.getStateInformation(again);
    CHECK(again == blob); // byte for byte
}

TEST_CASE("App: a cab IR changes the sound, and a missing IR only silences its slot", "[app][preset][cab]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    auto values = defaults();

    PluginProcessor none;
    none.loadPreset(makePreset("11111111-2222-4333-8444-555555555555", "No cab", values));
    const auto dry = render(none, 40);

    PluginProcessor withCab;
    withCab.prepareToPlay(48000.0, 256);
    CHECK(withCab.loadPreset(makePreset("11111111-2222-4333-8444-555555555555", "Cab", values, {{0, 0, kUri, ""}}))
              .empty());
    const auto wet = render(withCab, 40);
    CHECK(dry != wet);
    CHECK(std::all_of(wet.begin(), wet.end(), [](float v) { return std::isfinite(v); }));

    PluginProcessor missing;
    missing.prepareToPlay(48000.0, 256);
    const auto warnings = missing.loadPreset(
        makePreset("11111111-2222-4333-8444-555555555555", "Gone", values, {{0, 0, "factory:ir/does-not-exist", ""}}));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("cab.ch1.ir1") != std::string::npos);
    CHECK(presetValues(missing.capturePreset()) == values); // the rest of the preset still loaded
}

TEST_CASE("App: Save as starts a new preset identity", "[app][preset]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    PluginProcessor p;
    const auto before = p.presetId();
    p.beginNewPreset("Fresh");
    CHECK(p.presetName() == "Fresh");
    CHECK(p.presetId() != before);
    CHECK_FALSE(p.isDirty());
}

TEST_CASE("App: the IR installer gives replaced IRs back and never frees the engine's own", "[app][preset][cab]")
{
    Engine engine;
    CabIrInstaller installer;
    engine.prepare(48000.0, 256);
    std::vector<float> left(256, 0.0f);
    float* channels[2] = {left.data(), left.data()};

    const std::vector<float> ir(64, 0.01f);
    for (int round = 0; round < 3; ++round) {
        REQUIRE(installer.install(0, 0, ir));
        installer.pump(engine);
        for (int block = 0; block < 20; ++block) { // long enough for the fade to finish
            processChunked(engine, channels, 1, 256);
            installer.drain(engine);
        }
        installer.collect();
    }
    CHECK(installer.liveCount() ==
          1); // only the IR now installed is alive; the engine's "no cab" was never queued back

    REQUIRE(installer.installNone(engine, 0, 0)); // back to the engine's impulse
    installer.pump(engine);
    for (int block = 0; block < 20; ++block) {
        processChunked(engine, channels, 1, 256);
        installer.drain(engine);
    }
    installer.collect();
    CHECK(installer.liveCount() == 0);
}
