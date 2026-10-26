#pragma once

#include "osp/engine/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace osp::app {

// Thin JUCE wrapper around osp::engine::Engine. Threading follows ADR-0004 / docs/spec/threading.md:
// prepareToPlay and state calls are control-thread, processBlock is the audio thread.
class PluginProcessor final : public juce::AudioProcessor {
public:
    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // Control thread. JSON blob per preset-schema.md §8; parameters are written synchronously (threading.md §6).
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& parameters() noexcept { return parameters_; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState parameters_;
    osp::engine::Engine engine_;

    // Stand-in for the EngineController mutex (ADR-0004 rule 6): serialises state calls; never taken on the audio thread.
    juce::CriticalSection stateLock_;
    std::string presetId_;
    std::string presetName_ = "Init";
    bool dirty_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace osp::app
