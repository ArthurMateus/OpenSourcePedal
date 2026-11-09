#include "OspEditor.h"

namespace osp::ui {

OspEditor::OspEditor(osp::app::PluginProcessor& processor)
    : AudioProcessorEditor(processor),
      blocks_(processor.parameters()),
      page_(processor.parameters()),
      status_(processor)
{
    setLookAndFeel(&lookAndFeel_);
    addAndMakeVisible(blocks_);
    addAndMakeVisible(page_);
    addAndMakeVisible(status_);
    blocks_.onSelect = [this](int index) { page_.show(index); };
    blocks_.select(kAmpIndex);

    constrainer_.setFixedAspectRatio(static_cast<double>(kWidth) / kHeight);
    constrainer_.setMinimumSize(kWidth / 2, kHeight / 2);
    constrainer_.setMaximumSize(kWidth * 2, kHeight * 2);
    setConstrainer(&constrainer_);
    setResizable(true, true);
    setSize(kWidth, kHeight);
}

OspEditor::~OspEditor()
{
    setLookAndFeel(nullptr);
}

juce::Rectangle<int> OspEditor::pageArea() const
{
    return getLocalBounds()
        .withTrimmedTop(px(kTopBarHeight + kBlockRowHeight))
        .withTrimmedBottom(px(kStatusHeight))
        .withTrimmedRight(px(kSideWidth))
        .reduced(px(12));
}

void OspEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);

    g.setColour(colours::text);
    g.setFont(juce::Font(juce::FontOptions(static_cast<float>(px(20)), juce::Font::bold)));
    g.drawText("OpenSourcePedal", getLocalBounds().removeFromTop(px(kTopBarHeight)).reduced(px(12), 0),
               juce::Justification::centredLeft);
}

void OspEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(px(kTopBarHeight));
    blocks_.setBounds(area.removeFromTop(px(kBlockRowHeight)).reduced(px(6), px(4)));
    page_.setBounds(pageArea());
    status_.setBounds(getLocalBounds().removeFromBottom(px(kStatusHeight)));
}

} // namespace osp::ui
