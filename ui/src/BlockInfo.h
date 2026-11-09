#pragma once

#include <array>

namespace osp::ui {

// One block of the chain. The host parameters of a block are `<id>.*` (parameters.md §1.1); `<id>.enabled` exists
// for every block except the amp (always on) and the output.
struct BlockInfo {
    const char* id;
    const char* name;
    bool hasEnabled;
};

// Signal order (signal-flow.md §1). The modulation block is shown at its default post-cab position: its pre-amp
// position is a preset-only option.
inline constexpr std::array<BlockInfo, 13> kBlocks{{
    {"input", "Input", true},
    {"gate", "Gate", true},
    {"comp", "Comp", true},
    {"drive1", "Drive 1", true},
    {"drive2", "Drive 2", true},
    {"amp", "Amp", false},
    {"cab", "Cab", true},
    {"eq", "EQ", true},
    {"mod", "Mod", true},
    {"delay", "Delay", true},
    {"reverb", "Reverb", true},
    {"doubler", "Doubler", true},
    {"output", "Output", false},
}};

inline constexpr int kAmpIndex = 5;

} // namespace osp::ui
