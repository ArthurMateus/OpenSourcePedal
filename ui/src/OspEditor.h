#pragma once

#include "BlockStrip.h"
#include "OspLookAndFeel.h"
#include "PluginProcessor.h"

namespace osp::ui {

// The minimal editor (issue #198). Layout, top to bottom: a top bar (title; the preset menu goes here, #199), the block
// row, then the page of the selected block. A side panel for the tone-match controls (#199) takes its width from
// kSideWidth, which is 0 until then. All sizes are in design pixels (kWidth x kHeight) and scale with the window.
class OspEditor final : public juce::AudioProcessorEditor {
public:
    static constexpr int kWidth = 1000, kHeight = 620;
    static constexpr int kTopBarHeight = 40, kBlockRowHeight = 84, kSideWidth = 0;

    explicit OspEditor(osp::app::PluginProcessor& processor);
    ~OspEditor() override;

    BlockStrip& blocks() noexcept { return blocks_; }
    int selectedBlock() const noexcept { return blocks_.selected(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Design pixels to window pixels.
    int px(int designPixels) const noexcept
    {
        return juce::roundToInt(static_cast<float>(designPixels) * static_cast<float>(getWidth()) / kWidth);
    }
    juce::Rectangle<int> pageArea() const;

    OspLookAndFeel lookAndFeel_;
    BlockStrip blocks_;
    juce::ComponentBoundsConstrainer constrainer_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OspEditor)
};

} // namespace osp::ui
