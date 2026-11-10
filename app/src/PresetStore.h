#pragma once

// Preset files on disk (preset-schema.md §1): the factory folder and the user's folder. No JUCE: std::filesystem only,
// so the unit tests need no message loop. Control thread only (file I/O).

#include "osp/engine/Preset.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace osp::app {

struct PresetEntry {
    std::string name;     // from the document, shown in the menu
    std::string category; // "" for user presets
    std::filesystem::path path;
    bool factory = false;
};

// %APPDATA%\OpenSourcePedal\Presets\User, or empty when the environment has no app-data folder.
std::filesystem::path defaultUserPresetDir();

// The folder with the factory presets (presets/factory): the first of `candidates` that exists, else empty. The editor
// passes <exe dir>/presets/factory and the source tree's folder, so a build from the repository finds them.
std::filesystem::path findFactoryPresetDir(const std::vector<std::filesystem::path>& candidates);

// Every readable preset under the two folders (either may be empty or missing: then it has none), factory first, each
// group sorted by category then name. Files that are not valid v1 presets are skipped; `skipped` counts them.
std::vector<PresetEntry>
listPresets(const std::filesystem::path& factoryDir, const std::filesystem::path& userDir, int* skipped = nullptr);

// Reads and parses one file. nullopt with the reason in *error (unreadable, too large, not a preset).
std::optional<osp::engine::Preset> loadPresetFile(const std::filesystem::path& path, std::string* error = nullptr);

// File name for a preset name: lower case letters and digits, other runs become one '-', at most 48 characters;
// "preset" when nothing is left.
std::string presetFileSlug(const std::string& name);

// Writes `<userDir>/<slug>.osp.json` (creating the folder). An existing file with the same preset id is replaced; a
// file of another preset with the same slug is kept and the new one gets -2, -3, ... The write goes to a temporary file
// that is renamed, so a failure never leaves half a preset. Returns the path, or nullopt with *error.
std::optional<std::filesystem::path>
saveUserPreset(const std::filesystem::path& userDir, const osp::engine::Preset& preset, std::string* error = nullptr);

} // namespace osp::app
