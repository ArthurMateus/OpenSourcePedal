#pragma once

// Engine-internal (nlohmann stays out of the public headers, ADR-0006 D3): the preset document as a JSON value, so
// State.cpp can embed it in the plugin state blob.

#include "osp/engine/Preset.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace osp::engine {

// Nesting depth of objects and arrays, ignoring brackets inside strings.
int jsonNestingDepth(std::string_view text) noexcept;
// Throws nlohmann::json::exception for a wrong type; nullopt + *error for a document that is not a v1 preset.
std::optional<Preset> presetFromJson(const nlohmann::json& doc, std::string* error);
nlohmann::json presetToJson(const Preset& preset);

} // namespace osp::engine
