#pragma once

#include <span>

namespace osp::ui {

// A note is read-only text, not a parameter: `label` is its text.
enum class ControlKind { knob, toggle, choice, note };

// One control of a block page, bound to the host parameter `<block id>.<key>`. In a key, "{ch}" stands for the active
// amp channel (the cab and EQ parameters are per channel, parameters.md §3.1 and §3.2).
struct ControlSpec {
    ControlKind kind;
    const char* key;
    const char* label;
};

// The main controls of a block (an index into kBlocks), in display order.
std::span<const ControlSpec> controlsFor(int blockIndex);

} // namespace osp::ui
