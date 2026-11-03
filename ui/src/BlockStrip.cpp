#include "BlockStrip.h"

#include "OspLookAndFeel.h"

namespace osp::ui {

BlockTile::BlockTile(juce::AudioProcessorValueTreeState& state, const BlockInfo& info, std::function<void()> onSelect)
    : info_(info),
      onSelect_(std::move(onSelect))
{
    if (info.hasEnabled) {
        addAndMakeVisible(power_);
        attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            state, juce::String(info.id) + ".enabled", power_);
    }
}

void BlockTile::setSelected(bool selected)
{
    selected_ = selected;
    repaint();
}

void BlockTile::resized()
{
    const int size = juce::jmin(16, getHeight() / 3);
    power_.setBounds(getLocalBounds().removeFromTop(getHeight() / 2).withSizeKeepingCentre(size, size));
}

void BlockTile::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(colours::panel);
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour(selected_ ? colours::accent : colours::outline);
    g.drawRoundedRectangle(bounds, 6.0f, selected_ ? 2.0f : 1.0f);

    g.setColour(selected_ ? colours::text : colours::dimText);
    g.setFont(juce::Font(juce::FontOptions(juce::jmin(15.0f, static_cast<float>(getHeight()) * 0.2f))));
    g.drawText(info_.name, getLocalBounds().removeFromBottom(getHeight() / 2), juce::Justification::centred);
}

void BlockTile::mouseDown(const juce::MouseEvent&)
{
    if (onSelect_)
        onSelect_();
}

BlockStrip::BlockStrip(juce::AudioProcessorValueTreeState& state)
{
    for (std::size_t i = 0; i < kBlocks.size(); ++i) {
        auto tile = std::make_unique<BlockTile>(state, kBlocks[i], [this, i] { select(static_cast<int>(i)); });
        addAndMakeVisible(*tile);
        tiles_.push_back(std::move(tile));
    }
}

void BlockStrip::select(int index)
{
    if (index == selected_)
        return;
    selected_ = index;
    for (std::size_t i = 0; i < tiles_.size(); ++i)
        tiles_[i]->setSelected(static_cast<int>(i) == index);
    if (onSelect)
        onSelect(index);
}

void BlockStrip::resized()
{
    auto area = getLocalBounds();
    const int count = static_cast<int>(tiles_.size());
    for (int i = 0; i < count; ++i)
        tiles_[static_cast<std::size_t>(i)]->setBounds(area.removeFromLeft(area.getWidth() / (count - i)));
}

} // namespace osp::ui
