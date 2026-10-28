// ADR-0009 D8 / PR 8: the host parameter layout comes from the engine table, and every host atomic is bound into the
// engine's ParamStore. The app reports the graph's L_total.

#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <cmath>

namespace {

using namespace osp::engine;

int hostIndex(int tableIndex)
{
    int n = 0;
    for (int i = 0; i < tableIndex; ++i)
        n += paramTable()[static_cast<std::size_t>(i)].vis == Visibility::host ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("App: the host sees the 226 table parameters, in table order, at their defaults", "[app][params]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;

    const auto& params = p.getParameters();
    REQUIRE(params.size() == hostParamCount());
    REQUIRE(params.size() == 226);

    const auto table = paramTable();
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& info = table[i];
        if (info.vis != Visibility::host) {
            CHECK(p.parameters().getParameter(juce::String(info.id)) == nullptr);
            continue;
        }
        CAPTURE(info.id);
        auto* param = dynamic_cast<juce::RangedAudioParameter*>(params[hostIndex(static_cast<int>(i))]);
        REQUIRE(param != nullptr);
        CHECK(param->getParameterID() == juce::String(info.id));
        CHECK(p.parameters().getRawParameterValue(juce::String(info.id))->load() == info.def);
        // The engine reads the same value through the binding.
        CHECK(p.engine().parameters().get(static_cast<int>(i)) == info.def);
        CHECK(param->getDefaultValue() == param->convertTo0to1(info.def));
    }
}

TEST_CASE("App: a host change reaches the ParamStore, with the table's mapping", "[app][params]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    osp::app::PluginProcessor p;

    const auto table = paramTable();
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& info = table[i];
        if (info.vis != Visibility::host)
            continue;
        CAPTURE(info.id);
        auto* param = p.parameters().getParameter(juce::String(info.id));
        for (const float x : {0.0f, 0.37f, 1.0f}) {
            CAPTURE(x);
            param->setValueNotifyingHost(x);
            const float got = p.engine().parameters().get(static_cast<int>(i));
            // Host values snap to 6 significant digits, at most 1e-4 (bool/choice to their step), so allow that much.
            CHECK(std::abs(got - fromNormalized(info, param->getValue())) <= 1.0e-4f * std::max(1.0f, std::abs(got)));
            CHECK(got >= info.min);
            CHECK(got <= info.max);
        }
        param->setValueNotifyingHost(param->getDefaultValue());
    }
}

TEST_CASE("App: reported latency is the graph's L_total at every rate", "[app][params][latency]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    for (const double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        CAPTURE(fs);
        osp::app::PluginProcessor p;
        p.prepareToPlay(fs, 64);
        CHECK(p.getLatencySamples() == p.engine().getLatencySamples());
    }
}
