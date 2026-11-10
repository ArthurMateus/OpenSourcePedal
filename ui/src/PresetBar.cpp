#include "PresetBar.h"

#include "OspLookAndFeel.h"

#include <algorithm>
#include <map>

namespace osp::ui {

namespace fs = std::filesystem;
using osp::app::PresetEntry;

namespace {

std::string trimmed(const std::string& s)
{
    return juce::String::fromUTF8(s.c_str()).trim().toStdString();
}

fs::path toPath(const juce::File& f)
{
    return fs::path(f.getFullPathName().toWideCharPointer());
}

fs::path findFactoryDir()
{
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    return osp::app::findFactoryPresetDir(
        {toPath(exe.getChildFile("presets").getChildFile("factory")),
         toPath(exe.getParentDirectory().getChildFile("presets").getChildFile("factory")),
         fs::path(OSP_SOURCE_PRESET_DIR)});
}

} // namespace

PresetController::PresetController(osp::app::PluginProcessor& processor, fs::path factoryDir, fs::path userDir)
    : processor_(processor),
      factoryDir_(std::move(factoryDir)),
      userDir_(std::move(userDir))
{
    refresh();
}

void PresetController::refresh()
{
    entries_ = osp::app::listPresets(factoryDir_, userDir_, &skipped_);
}

std::string PresetController::load(std::size_t index)
{
    if (index >= entries_.size())
        return "No such preset";
    const auto& entry = entries_[index];
    std::string error;
    const auto preset = osp::app::loadPresetFile(entry.path, &error);
    if (!preset)
        return "Cannot load " + entry.name + ": " + error;
    const auto warnings = processor_.loadPreset(*preset);
    if (warnings.empty())
        return "Loaded " + entry.name;
    return "Loaded " + entry.name + " (" + std::to_string(warnings.size()) + " warning(s): " + warnings.front() + ")";
}

std::string PresetController::saveAs(const std::string& name)
{
    const auto clean = trimmed(name);
    if (clean.empty())
        return "Give the preset a name";
    if (userDir_.empty())
        return "No user preset folder";
    processor_.beginNewPreset(clean);
    std::string error;
    const auto path = osp::app::saveUserPreset(userDir_, processor_.capturePreset(), &error);
    if (!path)
        return "Cannot save: " + error;
    refresh();
    return "Saved " + clean;
}

bool PresetController::currentIsFactory() const
{
    const auto name = processor_.presetName();
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const PresetEntry& e) { return e.factory && e.name == name; });
}

std::string PresetController::save()
{
    if (currentIsFactory() || processor_.presetName() == "Init")
        return {}; // nothing of the user's to overwrite: the bar asks for a name
    if (userDir_.empty())
        return "No user preset folder";
    std::string error;
    const auto path = osp::app::saveUserPreset(userDir_, processor_.capturePreset(), &error);
    if (!path)
        return "Cannot save: " + error;
    refresh();
    return "Saved " + processor_.presetName();
}

PresetBar::PresetBar(osp::app::PluginProcessor& processor)
    : processor_(processor),
      controller_(processor, findFactoryDir(), osp::app::defaultUserPresetDir())
{
    menuButton_.onClick = [this] { showMenu(); };
    saveButton_.setButtonText("Save");
    saveButton_.onClick = [this] {
        const auto message = controller_.save();
        if (message.empty())
            askName();
        else
            report(message);
        refresh();
    };
    message_.setColour(juce::Label::textColourId, colours::dimText);
    message_.setMinimumHorizontalScale(0.7f);
    addAndMakeVisible(menuButton_);
    addAndMakeVisible(saveButton_);
    addAndMakeVisible(message_);
    refresh();
    startTimerHz(4);
}

PresetBar::~PresetBar() = default;

void PresetBar::refresh()
{
    const auto text = juce::String::fromUTF8(processor_.presetName().c_str()) + (processor_.isDirty() ? " *" : "");
    if (menuButton_.getButtonText() != text)
        menuButton_.setButtonText(text);
}

void PresetBar::report(const std::string& message)
{
    message_.setText(juce::String::fromUTF8(message.c_str()), juce::dontSendNotification);
}

void PresetBar::showMenu()
{
    controller_.refresh();
    juce::PopupMenu menu;
    std::map<std::string, juce::PopupMenu> categories;
    juce::PopupMenu user;
    const auto& entries = controller_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const int id = static_cast<int>(i) + 1;
        const auto name = juce::String::fromUTF8(entries[i].name.c_str());
        if (entries[i].factory)
            categories[entries[i].category.empty() ? "Other" : entries[i].category].addItem(id, name);
        else
            user.addItem(id, name);
    }
    for (auto& [category, sub] : categories)
        menu.addSubMenu(juce::String::fromUTF8(category.c_str()), sub);
    if (categories.empty())
        menu.addItem(-1, "No factory presets found", false);
    menu.addSeparator();
    if (user.getNumItems() > 0)
        menu.addSubMenu("User", user);
    else
        menu.addItem(-2, "No user presets yet", false);
    menu.addSeparator();
    constexpr int kSaveAs = 100000;
    menu.addItem(kSaveAs, "Save as...");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton_), [this](int chosen) {
        if (chosen == kSaveAs) {
            askName();
        } else if (chosen > 0) {
            report(controller_.load(static_cast<std::size_t>(chosen - 1)));
            refresh();
        }
    });
}

void PresetBar::askName()
{
    dialog_ = std::make_unique<juce::AlertWindow>("Save preset",
                                                  "Name of the new preset:", juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor(
        "name", processor_.presetName() == "Init" ? "" : juce::String::fromUTF8(processor_.presetName().c_str()));
    dialog_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
                                 if (result == 1 && dialog_ != nullptr)
                                     report(controller_.saveAs(dialog_->getTextEditorContents("name").toStdString()));
                                 dialog_.reset();
                                 refresh();
                             }));
}

void PresetBar::resized()
{
    auto area = getLocalBounds();
    saveButton_.setBounds(area.removeFromRight(juce::jmax(60, area.getWidth() / 8)).reduced(2, 4));
    menuButton_.setBounds(area.removeFromLeft(area.getWidth() * 3 / 5).reduced(2, 4));
    message_.setBounds(area.reduced(6, 4));
}

} // namespace osp::ui
