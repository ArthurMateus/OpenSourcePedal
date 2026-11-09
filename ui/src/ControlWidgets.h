#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace osp::ui {

using Apvts = juce::AudioProcessorValueTreeState;

// A labelled control bound to one host parameter through a JUCE attachment. The attachments run on the message thread;
// the audio thread only sees the parameter's atomic value.
class KnobControl final : public juce::Component {
public:
    KnobControl(Apvts& state, const juce::String& paramId, const juce::String& label);
    void resized() override;

private:
    juce::Label label_;
    juce::Slider slider_;
    std::unique_ptr<Apvts::SliderAttachment> attachment_;
};

class ToggleControl final : public juce::Component {
public:
    ToggleControl(Apvts& state, const juce::String& paramId, const juce::String& label);
    void resized() override;

private:
    juce::ToggleButton button_;
    std::unique_ptr<Apvts::ButtonAttachment> attachment_;
};

class ChoiceControl final : public juce::Component {
public:
    ChoiceControl(Apvts& state, const juce::String& paramId, const juce::String& label);
    void resized() override;

private:
    juce::Label label_;
    juce::ComboBox box_;
    std::unique_ptr<Apvts::ComboBoxAttachment> attachment_;
};

} // namespace osp::ui
