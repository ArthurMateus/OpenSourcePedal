#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3).

#include <string_view>

namespace osp::engine {

// Project version from CMake (project(... VERSION ...)), e.g. "0.1.0".
std::string_view versionString() noexcept;

} // namespace osp::engine
