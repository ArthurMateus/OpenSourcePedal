#pragma once

#include "BlockInfo.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

namespace osp::ui {

// One block of the row: a power LED bound to `<id>.enabled` and the block name. Clicking the tile selects the block.
class BlockTile final : public juce::Component {
public:
    BlockTile(juce::AudioProcessorValueTreeState& state, const BlockInfo& info, std::function<void()> onSelect);

    void setSelected(bool selected);
    juce::ToggleButton& power() noexcept { return power_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    const BlockInfo& info_;
    std::function<void()> onSelect_;
    juce::ToggleButton power_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment_;
    bool selected_ = false;
};

// The blocks in signal order, left to right.
class BlockStrip final : public juce::Component {
public:
    explicit BlockStrip(juce::AudioProcessorValueTreeState& state);

    // The selected block as an index into kBlocks; -1 selects nothing.
    void select(int index);
    int selected() const noexcept { return selected_; }
    BlockTile& tile(int index) { return *tiles_[static_cast<std::size_t>(index)]; }
    std::function<void(int)> onSelect;

    void resized() override;

private:
    std::vector<std::unique_ptr<BlockTile>> tiles_;
    int selected_ = -1;
};

} // namespace osp::ui
