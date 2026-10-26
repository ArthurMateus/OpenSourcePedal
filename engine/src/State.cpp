#include "osp/engine/State.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>

namespace osp::engine {

namespace {

constexpr std::size_t kMaxBytes = 1u << 20; // 1 MiB
constexpr int kMaxDepth = 16;

// Nesting depth of objects/arrays, ignoring brackets inside strings. Checked before parsing (ADR-0003 item 10).
int nestingDepth(std::string_view text) noexcept
{
    int depth = 0, maxDepth = 0;
    bool inString = false, escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            maxDepth = std::max(maxDepth, ++depth);
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return maxDepth;
}

} // namespace

std::string writeState(const PluginState& state)
{
    // nlohmann::json objects are std::map-backed, so dump() emits keys in sorted order.
    const nlohmann::json preset = {
        {"format", "osp-preset"},
        {"schemaVersion", 1},
        {"id", state.presetId},
        {"name", state.presetName},
        {"blocks", nlohmann::json::array({{{"type", "output"},
                                           {"id", "output"},
                                           {"enabled", true},
                                           {"params", {{"levelDb", state.outputLevelDb}}}}})},
    };
    const nlohmann::json blob = {
        {"format", "osp-state"},
        {"stateVersion", 1},
        {"preset", preset},
        {"source", {{"presetId", state.presetId}, {"dirty", state.dirty}}},
        {"ui", nlohmann::json::object()},
    };
    return blob.dump();
}

std::optional<PluginState> readState(std::string_view json) noexcept
{
    if (json.size() > kMaxBytes || nestingDepth(json) > kMaxDepth)
        return std::nullopt;
    try {
        const auto blob = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
        if (blob.is_discarded() || blob.value("format", "") != "osp-state" || blob.value("stateVersion", 0) != 1)
            return std::nullopt;

        const auto& preset = blob.at("preset");
        if (preset.value("format", "") != "osp-preset" || preset.value("schemaVersion", 0) != 1)
            return std::nullopt;

        PluginState state;
        state.presetId = preset.at("id").get<std::string>();
        state.presetName = preset.at("name").get<std::string>();
        state.dirty = blob.at("source").value("dirty", false);
        for (const auto& block : preset.at("blocks"))
            if (block.value("id", "") == "output")
                state.outputLevelDb = block.at("params").value("levelDb", 0.0);
        return state;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt; // missing field or wrong type
    }
}

} // namespace osp::engine
