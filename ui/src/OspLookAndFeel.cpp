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

void OspLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                      int x,
                                      int y,
                                      int width,
                                      int height,
                                      float position,
                                      float startAngle,
                                      float endAngle,
                                      juce::Slider&)
{
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(6.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const float arcRadius = radius - 3.0f;
    const float angle = startAngle + position * (endAngle - startAngle);

    juce::Path track, value;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
    g.setColour(colours::outline);
    g.strokePath(track, juce::PathStrokeType(4.0f));
    g.setColour(colours::accent);
    g.strokePath(value, juce::PathStrokeType(4.0f));

    juce::Path pointer;
    pointer.addRectangle(-1.5f, -arcRadius + 6.0f, 3.0f, arcRadius * 0.45f);
    g.setColour(colours::text);
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre));
}

void OspLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    const bool hasText = button.getButtonText().isNotEmpty();
    const float size = juce::jmin(bounds.getWidth(), bounds.getHeight(), 16.0f);
    const auto led = hasText ? bounds.removeFromLeft(size).withSizeKeepingCentre(size, size)
                             : bounds.withSizeKeepingCentre(size, size);
    g.setColour(button.getToggleState() ? colours::accent : colours::outline);
    g.fillEllipse(led);
    g.setColour(highlighted ? colours::text : colours::dimText);
    g.drawEllipse(led, 1.0f);
    if (hasText) {
        g.setColour(button.findColour(juce::ToggleButton::textColourId));
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText(button.getButtonText(), bounds.withTrimmedLeft(6.0f), juce::Justification::centredLeft);
    }
}

} // namespace osp::ui
