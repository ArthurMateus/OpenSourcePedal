#pragma once

// Preset documents (ADR-0003, docs/spec/preset-schema.md): preset.v1 JSON <-> parameter values. Public header:
// standard types only. Control thread only (these functions allocate; ADR-0004 rule 2).
//
// A preset names parameters by their full id (`amp.ch2.gain`, `cab.ch1.ir1.levelDb`, parameters.md). The v1 file maps
// onto them like this: block id + "." + key; an amp channel i is the strip of its builtin model (clean_a = 1,
// crunch_a = 2, lead_a = 3, as the graph builds them, else strip i + 1); a per-channel block (cab, eq) channel i
// follows the amp channel i; `enabled` is `<block>.enabled`; `activeChannel` is `amp.channel`; enum strings are
// choice names. Cab IR resources are returned separately because they are objects, not parameters.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osp::engine {

struct PresetParam {
    std::string id;
    double value = 0.0; // plain value: a bool is 0/1 and a choice its index
};

// The IR of cab.ch<channel + 1>.ir<slot + 1>. `uri` is factory:ir/<name> or file:///...; sha256 for non-factory.
struct PresetCab {
    int channel = 0, slot = 0;
    std::string uri, sha256;
};

struct Preset {
    std::string id, name = "Init", category, author;
    std::vector<PresetParam> params; // the values the document sets (everything else keeps its default)
    std::vector<PresetCab> cabs;
    std::vector<std::string> warnings; // what the reader skipped: unknown ids, unknown choice names, non-builtin amps
};

constexpr std::size_t kMaxPresetBytes = 1u << 20;

// nullopt (and the reason in *error) for malformed JSON, a wrong format or schemaVersion, a missing id, name or
// blocks, > 1 MiB or nesting deeper than 16 (ADR-0003 item 10). Never throws.
std::optional<Preset> parsePreset(std::string_view json, std::string* error = nullptr) noexcept;

// Canonical preset.v1 JSON (sorted keys), one block per table block. Writes every id in `preset.params` that is in
// the parameter table, so a preset made by makePreset() is a complete snapshot.
std::string writePreset(const Preset& preset);

// One value per parameter table entry, in table order: the defaults, then the preset's values, sanitized.
std::vector<float> presetValues(const Preset& preset);

// A complete preset from `values` (table order, size == table size; a shorter span leaves the rest at defaults).
Preset makePreset(std::string id, std::string name, std::span<const float> values, std::vector<PresetCab> cabs = {});

} // namespace osp::engine
