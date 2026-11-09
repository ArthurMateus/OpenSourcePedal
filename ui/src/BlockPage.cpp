#include "BlockPage.h"

#include "BlockInfo.h"

namespace osp::ui {

BlockPage::BlockPage(Apvts& state)
    : state_(state),
      channelAttachment_(*state.getParameter("amp.channel"), [this](float plain) {
          // amp.channel is a choice, so the plain value is the 0-based index (1 = "channel 1" in the host's 1-based
          // id).
          const int channel = static_cast<int>(plain) + 1;
          if (channel == channel_)
              return;
          channel_ = channel;
          rebuild();
      })
{
    channelAttachment_.sendInitialUpdate();
}

void BlockPage::show(int blockIndex)
{
    block_ = blockIndex;
    rebuild();
}

void BlockPage::rebuild()
{
    controls_.clear();
    if (block_ < 0)
        return;
    for (const auto& spec : controlsFor(block_)) {
        const auto id = juce::String(kBlocks[static_cast<std::size_t>(block_)].id) + "." +
                        juce::String(spec.key).replace("{ch}", juce::String(channel_));
        std::unique_ptr<juce::Component> control;
        switch (spec.kind) {
        case ControlKind::knob:
            control = std::make_unique<KnobControl>(state_, id, spec.label);
            break;
        case ControlKind::toggle:
            control = std::make_unique<ToggleControl>(state_, id, spec.label);
            break;
        case ControlKind::choice:
            control = std::make_unique<ChoiceControl>(state_, id, spec.label);
            break;
        }
        addAndMakeVisible(*control);
        controls_.push_back(std::move(control));
    }
    resized();
}

void BlockPage::resized()
{
    // Cells are square, up to 8 across, so the grid scales with the window.
    const int columns = 8;
    const int cell = getWidth() / columns;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const int col = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
        controls_[i]->setBounds(col * cell, row * cell, cell, cell);
    }
}

} // namespace osp::ui
