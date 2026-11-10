#include "osp/engine/State.h"

#include "PresetJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>

namespace osp::engine {

namespace {

constexpr std::size_t kMaxBytes = 1u << 20; // 1 MiB
constexpr int kMaxDepth = 16;

} // namespace

std::string writeState(const PluginState& state)
{
    // nlohmann::json objects are std::map-backed, so dump() emits keys in sorted order.
    if (!state.params.empty()) { // a complete snapshot (every parameter and cab IR), see Preset.h
        Preset full;
        full.id = state.presetId;
        full.name = state.presetName;
        full.params = state.params;
        full.cabs = state.cabs;
        const nlohmann::json blob = {
            {"format", "osp-state"},
            {"stateVersion", 1},
            {"preset", presetToJson(full)},
            {"source", {{"presetId", state.presetId}, {"dirty", state.dirty}}},
            {"ui", nlohmann::json::object()},
        };
        return blob.dump();
    }
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
    if (json.size() > kMaxBytes || jsonNestingDepth(json) > kMaxDepth)
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
        // No "output" block (or no levelDb) means the default 0 dB, as in a preset that omits a block (v1).
        for (const auto& block : preset.at("blocks"))
            if (block.value("id", "") == "output")
                state.outputLevelDb = block.at("params").value("levelDb", 0.0);
        // Only a real snapshot is kept; an old output-only blob stays on the old path so it re-writes byte-identically.
        if (auto full = presetFromJson(preset, nullptr); full && (full->params.size() > 1 || !full->cabs.empty())) {
            state.params = std::move(full->params);
            state.cabs = std::move(full->cabs);
        }
        return state;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt; // missing field or wrong type
    } catch (const std::exception&) {
        return std::nullopt; // std::bad_alloc while parsing: noexcept must not turn it into std::terminate
    }
}

} // namespace osp::engine
