#include "PluginProcessor.h"

namespace osp::app {

namespace {
// Host-visible id = the preset parameter id `<block>.<key>` (preset-schema.md §3), so automation survives presets.
constexpr auto kGainId = "output.levelDb";
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::mono(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters_(*this, nullptr, "OpenSourcePedal", createParameterLayout())
{
    // The engine reads the host-owned atomic once per block (ADR-0004 rule 3).
    engine_.setOutputLevelParameter(parameters_.getRawParameterValue(kGainId));
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
