#pragma once

// Finds and decodes the IR file behind a preset's cab reference (preset-schema.md §3, `factory:ir/<name>` or
// `file:///...` plus sha256). Control thread: file I/O and allocation.

#include "osp/engine/Preset.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace osp::app {

struct DecodedIr {
    std::vector<float> samples; // first channel only
    double sampleRate = 0.0;
};

// The file a cab reference names: assets/cab/<name>.wav under `assetDir` for a factory id, the path for file:///.
// nullopt (reason in *error) for another scheme, a name with a path separator, or a missing file; a file: reference
// whose sha256 is present and differs is refused too.
std::optional<std::filesystem::path>
resolveCabFile(const osp::engine::PresetCab& cab, const std::filesystem::path& assetDir, std::string* error);

// Decodes a WAV/AIFF/FLAC file; nullopt (reason in *error) when unreadable, empty or longer than 10 s.
std::optional<DecodedIr> decodeIrFile(const std::filesystem::path& file, std::string* error);

} // namespace osp::app
