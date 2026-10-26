#include "PluginProcessor.h"

#include "osp/engine/State.h"

namespace osp::app {

namespace {
// Host-visible id = the preset parameter id `<block>.<key>` (preset-schema.md §3), so automation survives presets.
constexpr auto kGainId = "output.levelDb";
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_(*this, nullptr, "OpenSourcePedal", createParameterLayout()),
      presetId_(juce::Uuid().toDashedString().toLowerCase().toStdString())
{
    // The engine reads the host-owned atomic once per block (ADR-0004 rule 3).
    engine_.setGainParameter(parameters_.getRawParameterValue(kGainId));
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{kGainId, 1}, "Output Level",
                                                           juce::NormalisableRange<float>{-24.0f, 12.0f, 0.1f}, 0.0f,
                                                           juce::AudioParameterFloatAttributes{}.withLabel("dB")));
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
