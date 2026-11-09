#include "ControlWidgets.h"

#include "OspLookAndFeel.h"

namespace osp::ui {

namespace {
void styleLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setColour(juce::Label::textColourId, colours::dimText);
}
} // namespace

KnobControl::KnobControl(Apvts& state, const juce::String& paramId, const juce::String& label)
{
    styleLabel(label_, label);
    addAndMakeVisible(label_);
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    slider_.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(slider_);
    attachment_ = std::make_unique<Apvts::SliderAttachment>(state, paramId, slider_);
}

void KnobControl::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(juce::jmax(16, getHeight() / 6)));
    slider_.setBounds(area);
}

ToggleControl::ToggleControl(Apvts& state, const juce::String& paramId, const juce::String& label)
{
    button_.setButtonText(label);
    addAndMakeVisible(button_);
    attachment_ = std::make_unique<Apvts::ButtonAttachment>(state, paramId, button_);
}

void ToggleControl::resized()
{
    button_.setBounds(getLocalBounds().withSizeKeepingCentre(getWidth(), juce::jmin(getHeight(), 28)));
}

ChoiceControl::ChoiceControl(Apvts& state, const juce::String& paramId, const juce::String& label)
{
    styleLabel(label_, label);
    addAndMakeVisible(label_);
    if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(state.getParameter(paramId)))
        box_.addItemList(parameter->choices, 1);
    addAndMakeVisible(box_);
    attachment_ = std::make_unique<Apvts::ComboBoxAttachment>(state, paramId, box_);
}

void ChoiceControl::resized()
{
    auto area = getLocalBounds();
    label_.setBounds(area.removeFromTop(juce::jmax(16, getHeight() / 6)));
    box_.setBounds(area.withSizeKeepingCentre(area.getWidth() - 8, juce::jmin(area.getHeight(), 28)));
}

ModelNote::ModelNote(int channel)
{
    static constexpr const char* kModels[] = {"clean_a", "crunch_a", "lead_a"};
    text_ = juce::String("Model\n") + kModels[juce::jlimit(1, 3, channel) - 1] + "\n(preset)";
}

void ModelNote::paint(juce::Graphics& g)
{
    g.setColour(colours::dimText);
    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.drawFittedText(text_, getLocalBounds(), juce::Justification::centred, 3);
}

} // namespace osp::ui
