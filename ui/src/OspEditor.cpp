#include "OspEditor.h"

namespace osp::ui {

OspEditor::OspEditor(osp::app::PluginProcessor& processor)
    : AudioProcessorEditor(processor),
      blocks_(processor.parameters()),
      page_(processor.parameters()),
      status_(processor),
      presets_(processor)
{
    setLookAndFeel(&lookAndFeel_);
    addAndMakeVisible(blocks_);
    addAndMakeVisible(page_);
    addAndMakeVisible(status_);
    addAndMakeVisible(presets_);
    if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        toneMatch_ = std::make_unique<ToneMatchPanel>(
            processor, presets_.controller().userDir(),
            std::filesystem::path(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                      .getChildFile("OpenSourcePedal")
                                      .getChildFile("tonematch")
                                      .getFullPathName()
                                      .toWideCharPointer()));
        addAndMakeVisible(*toneMatch_);
    }
    blocks_.onSelect = [this](int index) { page_.show(index); };
    blocks_.select(kAmpIndex);

    constrainer_.setFixedAspectRatio(static_cast<double>(designWidth()) / kHeight);
    constrainer_.setMinimumSize(designWidth() / 2, kHeight / 2);
    constrainer_.setMaximumSize(designWidth() * 2, kHeight * 2);
    setConstrainer(&constrainer_);
    setResizable(true, true);
    setSize(designWidth(), kHeight);
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
        .withTrimmedRight(px(sideWidth()))
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
    presets_.setBounds(area.removeFromTop(px(kTopBarHeight)).withTrimmedLeft(px(230)).reduced(px(6), px(2)));
    blocks_.setBounds(area.removeFromTop(px(kBlockRowHeight)).reduced(px(6), px(4)));
    page_.setBounds(pageArea());
    if (toneMatch_)
        toneMatch_->setBounds(getLocalBounds()
                                  .withTrimmedTop(px(kTopBarHeight + kBlockRowHeight))
                                  .withTrimmedBottom(px(kStatusHeight))
                                  .removeFromRight(px(kSideWidth)));
    status_.setBounds(getLocalBounds().removeFromBottom(px(kStatusHeight)));
}

} // namespace osp::ui
