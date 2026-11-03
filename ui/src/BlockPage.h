#pragma once

#include "BlockControls.h"
#include "ControlWidgets.h"

#include <memory>
#include <vector>

namespace osp::ui {

// The page of the selected block: its main controls in a wrapping grid. Cab and EQ controls follow the active amp
// channel, so the page rebuilds when `amp.channel` changes.
class BlockPage final : public juce::Component {
public:
    explicit BlockPage(Apvts& state);

    // Index into kBlocks, or -1 for an empty page.
    void show(int blockIndex);
    int controlCount() const noexcept { return static_cast<int>(controls_.size()); }

    void resized() override;

private:
    void rebuild();

    Apvts& state_;
    int block_ = -1;
    int channel_ = 1;
    std::vector<std::unique_ptr<juce::Component>> controls_;
    juce::ParameterAttachment channelAttachment_;
};

} // namespace osp::ui
