#pragma once

#include "BlockPage.h"
#include "BlockStrip.h"
#include "OspLookAndFeel.h"
#include "PluginProcessor.h"
#include "PresetBar.h"
#include "StatusBar.h"
#include "ToneMatchPanel.h"

namespace osp::ui {

// The minimal editor (issue #198). Layout, top to bottom: a top bar (title and the preset menu), the block
// row, then the page of the selected block and, at the bottom, the status bar (meters, tuner, latency). The standalone
// app adds a side panel of kSideWidth for the tone-match controls. All sizes are in design
// pixels (kWidth x kHeight) and scale with the window.
class OspEditor final : public juce::AudioProcessorEditor {
public:
    static constexpr int kWidth = 1000, kHeight = 620;
    static constexpr int kTopBarHeight = 40, kBlockRowHeight = 84, kStatusHeight = 56, kSideWidth = 300;

    explicit OspEditor(osp::app::PluginProcessor& processor);
    ~OspEditor() override;

    BlockStrip& blocks() noexcept { return blocks_; }
    BlockPage& page() noexcept { return page_; }
    StatusBar& status() noexcept { return status_; }
    PresetBar& presets() noexcept { return presets_; }
    // The tone-match panel; only the standalone app has one (a plugin host owns the audio files), else nullptr.
    ToneMatchPanel* toneMatch() noexcept { return toneMatch_.get(); }
    int selectedBlock() const noexcept { return blocks_.selected(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Design pixels to window pixels.
    int px(int designPixels) const noexcept
    {
        return juce::roundToInt(static_cast<float>(designPixels) * static_cast<float>(getWidth()) / designWidth());
    }
    int sideWidth() const noexcept { return toneMatch_ ? kSideWidth : 0; }
    int designWidth() const noexcept { return kWidth + sideWidth(); }
    juce::Rectangle<int> pageArea() const;

    OspLookAndFeel lookAndFeel_;
    BlockStrip blocks_;
    BlockPage page_;
    StatusBar status_;
    PresetBar presets_;
    std::unique_ptr<ToneMatchPanel> toneMatch_;
    juce::ComponentBoundsConstrainer constrainer_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OspEditor)
};

} // namespace osp::ui
