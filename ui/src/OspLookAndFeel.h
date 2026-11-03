#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace osp::ui {

// Original flat dark styling on top of the stock JUCE look and feel: no bitmaps, no third-party art.
namespace colours {
inline const juce::Colour background{0xff16181c};
inline const juce::Colour panel{0xff23262c};
inline const juce::Colour outline{0xff3a3f48};
inline const juce::Colour text{0xffe6e8eb};
inline const juce::Colour dimText{0xff8d94a0};
inline const juce::Colour accent{0xfff0a53a};
} // namespace colours

class OspLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    OspLookAndFeel();

    // The on/off switch of a block: a small LED, lit while the block is on.
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

} // namespace osp::ui
