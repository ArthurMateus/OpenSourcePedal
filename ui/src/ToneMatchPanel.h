#pragma once

#include "PluginProcessor.h"
#include "ToneMatchJob.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <filesystem>
#include <memory>
#include <string>

namespace osp::ui {

// The side panel of the standalone app (issue #199, part 2): choose a reference (a file, or a link when yt-dlp and
// ffmpeg are installed) and a DI, run the tone match on a worker thread with progress and cancel, audition the result
// on the live sound and save it as a user preset. The match never runs on the audio or message thread.
class ToneMatchPanel final : public juce::Component, private juce::Timer {
public:
    enum class Phase { idle, fetching, matching, done, failed };

    // `userDir` is where Save puts the preset (the preset menu lists it); `workDir` holds downloads.
    ToneMatchPanel(osp::app::PluginProcessor& processor, std::filesystem::path userDir, std::filesystem::path workDir);
    ~ToneMatchPanel() override;

    // The inputs: a file path or an http(s) link, a DI file path, the analysed part of the reference in seconds or
    // m:ss (empty = whole), the preset name.
    void setReference(const juce::String& fileOrLink);
    void setDi(const juce::String& file);
    void setSection(const juce::String& start, const juce::String& end);
    void setPresetName(const juce::String& name);

    // Starts a run (downloading first for a link). Returns false, with the reason in statusText(), when an input is
    // missing or invalid or a run is going.
    bool run();
    void cancel();
    // Loads the result into the processor so it plays on the live input. False when there is no result.
    bool audition();
    // Writes the result to the user preset folder. False when there is no result or the write failed.
    bool saveResult();

    Phase phase() const noexcept { return phase_; }
    juce::String statusText() const { return status_.getText(); }
    juce::String warningsText() const { return warnings_.getText(); }
    double progress() const noexcept { return progressValue_; }
    // Reads the worker's state; the timer calls it, tests call it after waiting.
    void poll();

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff23262c)); }
    void resized() override;

private:
    class Fetch;

    void timerCallback() override { poll(); }
    void setStatus(const juce::String& text) { status_.setText(text, juce::dontSendNotification); }
    void startMatch(const std::filesystem::path& reference);
    void choose(juce::TextEditor& target, const juce::String& title);
    void updateButtons();

    osp::app::PluginProcessor& processor_;
    std::filesystem::path userDir_, workDir_;
    osp::app::ToneMatchJob job_;
    std::unique_ptr<Fetch> fetch_;
    Phase phase_ = Phase::idle;
    double progressValue_ = 0.0;

    juce::Label title_, legal_, referenceLabel_, diLabel_, sectionLabel_, nameLabel_, status_, warnings_;
    juce::TextEditor reference_, di_, start_, end_, name_;
    juce::TextButton chooseReference_{"..."}, chooseDi_{"..."}, run_{"Match"}, cancel_{"Cancel"}, audition_{"Audition"},
        save_{"Save preset"};
    juce::ToggleButton stem_{"Reference is an isolated guitar (keep drums filter off)"};
    juce::ProgressBar progressBar_{progressValue_};
    juce::HyperlinkButton guide_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String presetJson_;
};

} // namespace osp::ui
