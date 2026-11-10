#pragma once

// Test-only preset application (#200). The engine has no preset loader yet, so this maps a preset.v1 document onto the
// ParamStore by parameters.md ids and installs the factory cab IRs. Unknown parameters fail the test, so a typo in a
// factory preset is caught here.

#include "osp/dsp/conv/IrLoader.h"
#include "osp/dsp/conv/PartitionedConvolver.h"
#include "osp/engine/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <dr_wav.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace osp::test {

inline nlohmann::json readJson(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

inline std::vector<float> readMonoWav(const std::string& path, unsigned& rate)
{
    unsigned ch = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
    REQUIRE(raw != nullptr);
    std::vector<float> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i)
        x[i] = raw[i * ch];
    drwav_free(raw, nullptr);
    return x;
}

// An engine prepared at `rate` with a preset applied. Owns the cab convolvers it installs.
struct PresetRig {
    engine::Engine engine;
    std::vector<std::unique_ptr<dsp::PartitionedConvolver>> irs;

    static void set(engine::ParamStore& store, const std::string& id, const nlohmann::json& v)
    {
        const int index = engine::findParam(id);
        INFO("parameter " << id);
        REQUIRE(index >= 0);
        const auto& info = engine::paramTable()[static_cast<std::size_t>(index)];
        float plain = 0.0f;
        if (v.is_boolean()) {
            plain = v.get<bool>() ? 1.0f : 0.0f;
        } else if (v.is_string()) {
            const auto name = v.get<std::string>();
            const auto it = std::find(info.choices.begin(), info.choices.end(), std::string_view(name));
            INFO("choice " << name);
            REQUIRE(it != info.choices.end());
            plain = static_cast<float>(it - info.choices.begin());
        } else {
            plain = v.get<float>();
        }
        store.set(index, plain);
    }

    PresetRig(const nlohmann::json& preset, double rate, int maxBlock = 512)
    {
        auto& store = engine.parameters();
        std::vector<std::pair<std::pair<int, int>, std::string>> cabIrs; // (channel, slot) -> factory ir name
        for (const auto& block : preset.at("blocks")) {
            const auto id = block.at("id").get<std::string>();
            const auto type = block.at("type").get<std::string>();
            if (engine::findParam(id + ".enabled") >= 0)
                set(store, id + ".enabled", block.at("enabled"));
            if (type == "amp")
                store.set("amp.channel", block.at("activeChannel").get<float>());
            const auto channels = block.value("channels", nlohmann::json::array());
            for (std::size_t k = 0; k < channels.size(); ++k) {
                const std::string prefix = id + ".ch" + std::to_string(k + 1) + ".";
                const auto chParams = channels[k].value("params", nlohmann::json::object());
                const auto chResources = channels[k].value("resources", nlohmann::json::object());
                for (const auto& [key, v] : chParams.items())
                    set(store, prefix + key, v);
                for (const auto& [key, ref] : chResources.items()) {
                    const auto uri = ref.at("uri").get<std::string>();
                    REQUIRE(uri.rfind("factory:ir/", 0) == 0);
                    cabIrs.push_back({{static_cast<int>(k), key == "ir2" ? 1 : 0}, uri.substr(11)});
                }
            }
            const auto params = block.value("params", nlohmann::json::object());
            for (const auto& [key, v] : params.items())
                set(store, id + "." + key, v);
        }
        engine.prepare(rate, maxBlock);
        for (const auto& [slot, name] : cabIrs) {
            unsigned irRate = 0;
            const auto raw = readMonoWav(std::string(OSP_ASSET_DIR) + "/cab/" + name + ".wav", irRate);
            const auto ir = dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), irRate, rate);
            irs.push_back(std::make_unique<dsp::PartitionedConvolver>());
            irs.back()->prepare(ir.data(), static_cast<int>(ir.size()));
            engine.graph().setCabIr(slot.first, slot.second, irs.back().get());
            while (engine.graph().popRetired() != nullptr) {
            } // the engine owns its unit impulses
        }
        engine.reset();
    }

    // Renders `in` (mono) in blocks of 512 and returns the stereo output.
    void render(const std::vector<float>& in, std::vector<float>& l, std::vector<float>& r)
    {
        l = in;
        r.assign(in.size(), 0.0f);
        const int total = static_cast<int>(in.size());
        for (int pos = 0; pos < total; pos += 512) {
            float* chans[] = {l.data() + pos, r.data() + pos};
            engine::processChunked(engine, chans, 2, std::min(512, total - pos));
            while (engine.graph().popRetired() != nullptr) {
            }
        }
    }
};

} // namespace osp::test
