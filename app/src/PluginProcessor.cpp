#include "PluginProcessor.h"

#include "osp/engine/State.h"

#include <algorithm>
#include <cmath>

namespace osp::app {

namespace {
// Host-visible id = the preset parameter id `<block>.<key>` (preset-schema.md §3), so automation survives presets.
constexpr auto kGainId = "output.levelDb";

// Host values snap to 6 significant digits, never finer than 1e-4 of their unit, so save -> restore gives back the
// stored value exactly (REQ-902, ADR-0009 D10). A fixed 1e-4 grid would be finer than a float above about 1000
// (one float step at 6000 Hz is 4.9e-4), and a log-skew default would land a few ulps off.
float snapToGrid(const osp::engine::ParamInfo& p, float plain)
{
    const double v = plain;
    const double magnitude = std::abs(v) > 0.0 ? std::floor(std::log10(std::abs(v))) : 0.0;
    const double step = std::max(1.0e-4, std::pow(10.0, magnitude - 5.0));
    return osp::engine::sanitize(p, static_cast<float>(std::round(v / step) * step));
}

juce::String valueText(float plain, int)
{
    auto text = juce::String(plain, 4);
    if (text.containsChar('.'))
        text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text;
}

std::unique_ptr<juce::RangedAudioParameter> makeParameter(const osp::engine::ParamInfo& p)
{
    using namespace osp::engine;
    // Display names are the ids until the Phase 6 UI (ADR-0009 D10).
    const juce::ParameterID id{juce::String(p.id), 1};
    const juce::String name(p.id);
    switch (p.kind) {
    case ParamKind::boolean:
        return std::make_unique<juce::AudioParameterBool>(id, name, p.def > 0.5f);
    case ParamKind::choice: {
        juce::StringArray names;
        for (const auto choice : p.choices)
            names.add(juce::String(choice.data(), choice.size()));
        return std::make_unique<juce::AudioParameterChoice>(id, name, names, static_cast<int>(p.def));
    }
    case ParamKind::integer:
        return std::make_unique<juce::AudioParameterInt>(id, name, static_cast<int>(p.min), static_cast<int>(p.max),
                                                         static_cast<int>(p.def));
    case ParamKind::continuous:
        break;
    }
    // The table's skew (parameters.md §1.3) drives the host mapping, so normalized <-> plain matches the engine.
    // The table is immutable after first use, so capturing its entry by pointer is safe. The host -> plain mapping
    // snaps too: JUCE sets the default through a normalized round trip.
    const ParamInfo* info = &p;
    juce::NormalisableRange<float> range{
        p.min, p.max, [info](float, float, float x) { return snapToGrid(*info, fromNormalized(*info, x)); },
        [info](float, float, float v) { return toNormalized(*info, v); },
        [info](float, float, float v) { return snapToGrid(*info, v); }};
    juce::AudioParameterFloatAttributes attributes;
    attributes = attributes.withStringFromValueFunction(valueText);
    if (!p.unit.empty())
        attributes = attributes.withLabel(juce::String(p.unit.data(), p.unit.size()));
    return std::make_unique<juce::AudioParameterFloat>(id, name, range, p.def, attributes);
}
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_(*this, nullptr, "OpenSourcePedal", createParameterLayout()),
      presetId_(juce::Uuid().toDashedString().toLowerCase().toStdString())
{
    // The engine reads every host-owned atomic once per block (ADR-0004 rule 3, ADR-0009 D8). The raw value is the
    // plain value: a bool is 0/1 and a choice its index, as the ParamStore expects.
    const auto table = osp::engine::paramTable();
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i].vis == osp::engine::Visibility::host)
            engine_.parameters().bind(static_cast<int>(i), parameters_.getRawParameterValue(juce::String(table[i].id)));
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createParameterLayout()
{
    // Every host-visible entry of the v1 table (parameters.md §5: 226), in table order. Ranges come from the table, so
    // a restore that clamps through convertTo0to1() keeps every value the spec allows.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& p : osp::engine::paramTable())
        if (p.vis == osp::engine::Visibility::host)
            layout.add(makeParameter(p));
    return layout;
}

void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Control thread, audio stopped: the only place the engine may allocate.
    engine_.prepare(sampleRate, samplesPerBlock);
    setLatencySamples(engine_.getLatencySamples());
}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk && in.size() <= out.size();
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    // Mono input feeding a stereo output: duplicate the input before processing; clear any other extra outputs.
    for (int ch = numIn; ch < numOut; ++ch) {
        if (numIn == 1)
            buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);
        else
            buffer.clear(ch, 0, numSamples);
    }

    // Host blocks larger than the prepared size are split inside processChunked (ADR-0004 rule 8).
    osp::engine::processChunked(engine_, buffer.getArrayOfWritePointers(), numOut, numSamples);
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(stateLock_);
    osp::engine::PluginState state;
    state.presetId = presetId_;
    state.presetName = presetName_;
    state.outputLevelDb = parameters_.getRawParameterValue(kGainId)->load();
    state.dirty = dirty_;
    const auto blob = osp::engine::writeState(state);
    destData.replaceAll(blob.data(), blob.size());
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const juce::ScopedLock lock(stateLock_);
    if (data == nullptr || sizeInBytes <= 0)
        return;
    const auto state = osp::engine::readState({static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes)});
    if (!state)
        return; // invalid blob: keep the current state

    presetId_ = state->presetId;
    presetName_ = state->presetName;
    dirty_ = state->dirty;
    // Written synchronously, so getValue()/getStateInformation() on the next line see the restored value.
    auto* level = parameters_.getParameter(kGainId);
    level->setValueNotifyingHost(level->convertTo0to1(static_cast<float>(state->outputLevelDb)));
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

} // namespace osp::app

// JUCE plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new osp::app::PluginProcessor();
}
