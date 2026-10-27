#pragma once

// Plugin state blob (ADR-0003 item 9, docs/spec/preset-schema.md §8). Public header: standard types only.
// Control thread only: these functions allocate and never run on the audio thread (ADR-0004 rule 2).

#include <optional>
#include <string>
#include <string_view>

namespace osp::engine {

// v1 subset of the state: the pass-through engine has a single "output" block. Grows with the signal graph.
struct PluginState {
    std::string presetId;       // UUID v4, lowercase
    std::string presetName = "Init";
    double outputLevelDb = 0.0; // parameter "output.levelDb"
    bool dirty = false;
};

// UTF-8 JSON with keys in canonical (sorted) order, so equal states give byte-identical blobs (REQ-902).
std::string writeState(const PluginState& state);

// Returns nullopt for anything that is not a valid v1 "osp-state" blob: malformed JSON, wrong format or version,
// missing fields, > 1 MiB, or nesting deeper than 16 (ADR-0003 item 10). Never throws.
std::optional<PluginState> readState(std::string_view json) noexcept;

} // namespace osp::engine
