#include "OspLookAndFeel.h"

namespace osp::ui {

OspLookAndFeel::OspLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, colours::background);
    setColour(juce::Label::textColourId, colours::text);
    setColour(juce::TextButton::buttonColourId, colours::panel);
    setColour(juce::TextButton::textColourOffId, colours::text);
    setColour(juce::ToggleButton::textColourId, colours::text);
}

void OspLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    const float size = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto led = bounds.withSizeKeepingCentre(size, size);
    g.setColour(button.getToggleState() ? colours::accent : colours::outline);
    g.fillEllipse(led);
    g.setColour(highlighted ? colours::text : colours::dimText);
    g.drawEllipse(led, 1.0f);
}

} // namespace osp::ui
