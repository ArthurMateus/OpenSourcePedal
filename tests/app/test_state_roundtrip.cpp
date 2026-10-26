#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <vector>

namespace {

juce::MemoryBlock getState(osp::app::PluginProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation(block);
    return block;
}

void setLevel(osp::app::PluginProcessor& p, float db)
{
    auto* param = p.parameters().getParameter("output.levelDb");
    param->setValueNotifyingHost(param->convertTo0to1(db));
}

// Renders a fixed mono ramp through a stereo-out processor and returns the left channel.
std::vector<float> render(osp::app::PluginProcessor& p)
{
    p.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> buffer(2, 4800);
    juce::MidiBuffer midi;
    for (int n = 0; n < buffer.getNumSamples(); ++n)
        buffer.setSample(0, n, 0.001f * static_cast<float>(n % 300));
    p.processBlock(buffer, midi); // 4800 > 256, so this also goes through the block splitter
    return {buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples()};
}

} // namespace

TEST_CASE("App: state round-trips byte for byte and restores the audio", "[app][state]")
{
    const juce::ScopedJuceInitialiser_GUI juce;

    osp::app::PluginProcessor source;
    setLevel(source, -7.5f);
    const auto saved = getState(source);
    REQUIRE(saved == getState(source)); // two saves without a change are identical (REQ-902)

    osp::app::PluginProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    // Synchronous restore (threading.md §6): the very next calls see the restored value and bytes.
    CHECK(restored.parameters().getRawParameterValue("output.levelDb")->load() == -7.5f);
    REQUIRE(getState(restored) == saved);

    // REQ-PRE-009: save -> restore reproduces the audio output (identical here, so the null is -inf dBFS).
    REQUIRE(render(restored) == render(source));
}

TEST_CASE("App: the full output.levelDb range round-trips, including below the old -24 dB floor", "[app][state]")
{
    const juce::ScopedJuceInitialiser_GUI juce;

    // parameters.md §3.6: -60...+12 dB. -48 dB was clamped to -24 dB by the earlier NormalisableRange.
    for (const float db : {-60.0f, -48.0f, 12.0f}) {
        CAPTURE(db);
        osp::app::PluginProcessor source;
        setLevel(source, db);
        REQUIRE(source.parameters().getRawParameterValue("output.levelDb")->load() == db);
        const auto saved = getState(source);

        osp::app::PluginProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        CHECK(restored.parameters().getRawParameterValue("output.levelDb")->load() == db);
        REQUIRE(getState(restored) == saved);
    }
}

TEST_CASE("App: an invalid state blob is ignored and the current state kept", "[app][state]")
{
    const juce::ScopedJuceInitialiser_GUI juce;

    osp::app::PluginProcessor p;
    setLevel(p, 3.0f);
    const auto before = getState(p);

    const char garbage[] = "{\"format\":\"osp-state\",\"stateVersion\":1,\"preset\":";
    p.setStateInformation(garbage, static_cast<int>(sizeof(garbage) - 1));
    p.setStateInformation(nullptr, 0);
    REQUIRE(getState(p) == before);
}
