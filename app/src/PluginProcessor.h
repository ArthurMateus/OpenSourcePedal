#pragma once

#include "CabIrInstaller.h"

#include "osp/engine/Engine.h"
#include "osp/engine/Preset.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <filesystem>
#include <string>
#include <vector>

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
    // Control thread (tests): the engine whose ParamStore the host parameters are bound into.
    osp::engine::Engine& engine() noexcept { return engine_; }

    // Control thread. Applies every parameter of `preset` and installs its cab IRs (decoded and conditioned here, then
    // queued to the audio thread). The preset becomes the current one and clean. Returns what could not be applied
    // (unreadable IR, ...); a missing IR leaves its slot silent, the rest of the preset still loads.
    std::vector<std::string> loadPreset(const osp::engine::Preset& preset);
    // Control thread. A complete snapshot of the current sound under the current id and name.
    osp::engine::Preset capturePreset() const;
    // Control thread. Starts a new preset identity (new id, `name`, clean): "Save as".
    void beginNewPreset(const std::string& name);
    std::string presetName() const;
    std::string presetId() const;
    bool isDirty() const;
    // Control thread. The folder with cab/<name>.wav for `factory:ir/<name>`; found next to the executable by default.
    void setAssetDir(std::filesystem::path dir);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void applyPreset(const osp::engine::Preset& preset, bool dirty, std::vector<std::string>& warnings);
    void installCabs(std::vector<std::string>& warnings);

    juce::AudioProcessorValueTreeState parameters_;
    // Outlives the engine: its destructor lets go of installed IRs without deleting them.
    CabIrInstaller cabIrs_;
    osp::engine::Engine engine_;

    // Stand-in for the EngineController mutex (ADR-0004 rule 6): serialises state calls; never taken on the audio
    // thread.
    juce::CriticalSection stateLock_;
    std::string presetId_;
    std::string presetName_ = "Init";
    bool dirty_ = false;
    std::vector<osp::engine::PresetCab> cabs_;
    std::filesystem::path assetDir_;
    double sampleRate_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace osp::app
