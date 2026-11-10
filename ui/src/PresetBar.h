#pragma once

#include "PluginProcessor.h"
#include "PresetStore.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <filesystem>
#include <string>
#include <vector>

namespace osp::ui {

// What the preset menu does, without any widget: lists the factory and user presets, loads one into the processor,
// saves the current sound under a name. Message thread. Every call returns the one-line message the bar shows.
class PresetController {
public:
    PresetController(osp::app::PluginProcessor& processor,
                     std::filesystem::path factoryDir,
                     std::filesystem::path userDir);

    // Re-reads both folders.
    void refresh();
    const std::vector<osp::app::PresetEntry>& entries() const noexcept { return entries_; }

    std::string load(std::size_t index);
    // Saves under `name` as a new preset (new id) in the user folder. Empty or blank names are refused.
    std::string saveAs(const std::string& name);
    // Overwrites the current user preset. Returns "" when there is nothing to overwrite (a factory preset or an unsaved
    // Init): the caller asks for a name and calls saveAs().
    std::string save();
    // True when the current preset is one of the factory presets (by name).
    bool currentIsFactory() const;

    const std::filesystem::path& userDir() const noexcept { return userDir_; }

private:
    osp::app::PluginProcessor& processor_;
    std::filesystem::path factoryDir_, userDir_;
    std::vector<osp::app::PresetEntry> entries_;
    int skipped_ = 0;
};

// The top bar's preset controls: a button with the current preset name (opens the menu), Save, and a message line.
class PresetBar final : public juce::Component, private juce::Timer {
public:
    explicit PresetBar(osp::app::PluginProcessor& processor);
    ~PresetBar() override;

    PresetController& controller() noexcept { return controller_; }
    juce::String nameText() const { return menuButton_.getButtonText(); }
    juce::String messageText() const { return message_.getText(); }
    // Rebuilds the name and the dirty mark from the processor (a timer calls it; tests call it directly).
    void refresh();

    void resized() override;

private:
    void timerCallback() override { refresh(); }
    void showMenu();
    void askName();
    void report(const std::string& message);

    osp::app::PluginProcessor& processor_;
    PresetController controller_;
    juce::TextButton menuButton_, saveButton_;
    juce::Label message_;
    std::unique_ptr<juce::AlertWindow> dialog_;
};

} // namespace osp::ui
