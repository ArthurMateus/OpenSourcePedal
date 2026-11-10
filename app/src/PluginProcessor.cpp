#include "PluginProcessor.h"

#include "CabIrFiles.h"
#include "OspEditor.h"

#include "osp/dsp/conv/IrLoader.h"

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

    // The factory IRs sit in assets/ next to the executable (installer) or in the source tree (a build from the repo).
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    std::error_code ec;
    for (const auto& candidate : {exeDir.getChildFile("assets"), exeDir.getParentDirectory().getChildFile("assets"),
                                  juce::File(OSP_SOURCE_ASSET_DIR)}) {
        if (std::filesystem::is_directory(candidate.getFullPathName().toWideCharPointer(), ec)) {
            assetDir_ = candidate.getFullPathName().toWideCharPointer();
            break;
        }
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createParameterLayout()
{
    // Every host-visible entry of the v1 table (parameters.md §5: 229), in table order. Ranges come from the table, so
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
    // The installed IRs were conditioned for the old rate: condition them again for this one.
    const juce::ScopedLock lock(stateLock_);
    sampleRate_ = sampleRate;
    std::vector<std::string> warnings;
    installCabs(warnings);
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
    cabIrs_.pump(engine_);
    osp::engine::processChunked(engine_, buffer.getArrayOfWritePointers(), numOut, numSamples);
    cabIrs_.drain(engine_); // a finished fade retires the old IR
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::ScopedLock lock(stateLock_);
    osp::engine::PluginState state;
    state.presetId = presetId_;
    state.presetName = presetName_;
    state.outputLevelDb = parameters_.getRawParameterValue(kGainId)->load();
    state.dirty = dirty_;
    const auto snapshot = capturePreset();
    state.params = snapshot.params;
    state.cabs = snapshot.cabs;
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

    std::vector<std::string> warnings;
    if (!state->params.empty() || !state->cabs.empty()) {
        osp::engine::Preset preset;
        preset.id = state->presetId;
        preset.name = state->presetName;
        preset.params = state->params;
        preset.cabs = state->cabs;
        applyPreset(preset, state->dirty, warnings);
        return;
    }
    presetId_ = state->presetId;
    presetName_ = state->presetName;
    dirty_ = state->dirty;
    // Written synchronously, so getValue()/getStateInformation() on the next line see the restored value.
    auto* level = parameters_.getParameter(kGainId);
    level->setValueNotifyingHost(level->convertTo0to1(static_cast<float>(state->outputLevelDb)));
}

std::vector<std::string> PluginProcessor::loadPreset(const osp::engine::Preset& preset)
{
    const juce::ScopedLock lock(stateLock_);
    std::vector<std::string> warnings = preset.warnings;
    applyPreset(preset, false, warnings);
    return warnings;
}

osp::engine::Preset PluginProcessor::capturePreset() const
{
    const juce::ScopedLock lock(stateLock_);
    const auto size = osp::engine::paramTable().size();
    std::vector<float> values(size);
    for (std::size_t i = 0; i < size; ++i)
        values[i] = engine_.parameters().get(static_cast<int>(i)); // bound host atomics and preset-only values alike
    return osp::engine::makePreset(presetId_, presetName_, values, cabs_);
}

void PluginProcessor::beginNewPreset(const std::string& name)
{
    const juce::ScopedLock lock(stateLock_);
    presetId_ = juce::Uuid().toDashedString().toLowerCase().toStdString();
    presetName_ = name;
    dirty_ = false;
}

std::string PluginProcessor::presetName() const
{
    const juce::ScopedLock lock(stateLock_);
    return presetName_;
}

std::string PluginProcessor::presetId() const
{
    const juce::ScopedLock lock(stateLock_);
    return presetId_;
}

bool PluginProcessor::isDirty() const
{
    const juce::ScopedLock lock(stateLock_);
    return dirty_;
}

void PluginProcessor::setAssetDir(std::filesystem::path dir)
{
    const juce::ScopedLock lock(stateLock_);
    assetDir_ = std::move(dir);
}

void PluginProcessor::applyPreset(const osp::engine::Preset& preset, bool dirty, std::vector<std::string>& warnings)
{
    // Control thread, stateLock_ held. Host-visible values go through the host parameter (the engine reads the bound
    // atomic), preset-only values straight into the engine's store.
    const auto table = osp::engine::paramTable();
    const auto values = osp::engine::presetValues(preset);
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].vis == osp::engine::Visibility::host) {
            auto* p = parameters_.getParameter(juce::String(table[i].id));
            p->setValueNotifyingHost(p->convertTo0to1(values[i]));
        } else {
            engine_.parameters().set(static_cast<int>(i), values[i]);
        }
    }
    presetId_ = preset.id.empty() ? juce::Uuid().toDashedString().toLowerCase().toStdString() : preset.id;
    presetName_ = preset.name;
    dirty_ = dirty;
    cabs_ = preset.cabs;
    installCabs(warnings);
}

void PluginProcessor::installCabs(std::vector<std::string>& warnings)
{
    // Control thread, stateLock_ held. Every strip's two slots: the preset's IR, or none. Before prepareToPlay the
    // processing rate is unknown: prepareToPlay installs them.
    if (sampleRate_ <= 0.0)
        return;
    for (int channel = 0; channel < osp::engine::Graph::kNumChannels; ++channel) {
        for (int slot = 0; slot < 2; ++slot) {
            const auto it = std::find_if(cabs_.begin(), cabs_.end(), [&](const osp::engine::PresetCab& c) {
                return c.channel == channel && c.slot == slot;
            });
            bool installed = false;
            if (it != cabs_.end()) {
                std::string why;
                const auto file = resolveCabFile(*it, assetDir_, &why);
                const auto ir = file ? decodeIrFile(*file, &why) : std::nullopt;
                if (ir) {
                    const auto samples = osp::dsp::IrLoader::condition(
                        ir->samples.data(), static_cast<int>(ir->samples.size()), ir->sampleRate, sampleRate_);
                    installed = cabIrs_.install(channel, slot, samples);
                    if (!installed)
                        why = "IR queue full";
                }
                if (!installed)
                    warnings.push_back("cab.ch" + std::to_string(channel + 1) + ".ir" + std::to_string(slot + 1) +
                                       ": " + why);
            }
            if (!installed)
                cabIrs_.installNone(engine_, channel, slot);
        }
    }
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new osp::ui::OspEditor(*this);
}

} // namespace osp::app

// JUCE plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new osp::app::PluginProcessor();
}
