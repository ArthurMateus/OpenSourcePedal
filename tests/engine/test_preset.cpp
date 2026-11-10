#include "osp/engine/Params.h"
#include "osp/engine/Preset.h"
#include "osp/engine/State.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace osp::engine;

namespace {

constexpr auto kId = "3b0e8c52-6f1a-4c3e-9b7d-2a5f0e1c0f13";

// What the tone match writes (ToneMatch.cpp toPresetJson), reduced: one amp channel with the crunch model.
constexpr auto kToneMatchLike = R"({
  "blocks": [
    { "enabled": true, "id": "input", "params": { "gainDb": 3.5 }, "type": "input" },
    { "enabled": true, "id": "drive1", "params": { "drive": 4.0, "lowCutHz": 80.0, "type": "dist" }, "type": "drive" },
    { "activeChannel": 0, "channels": [ { "model": { "id": "osp.amp.crunch_a", "kind": "builtin" },
        "params": { "bass": 6.0, "gain": 7.25, "treble": 4.5 } } ], "enabled": true, "id": "amp", "type": "amp" },
    { "channels": [ { "params": { "ir1.levelDb": -2.0 }, "resources": { "ir1": { "uri": "factory:ir/412-a" } } } ],
      "enabled": true, "id": "cab", "perChannel": true, "type": "cab" },
    { "channels": [ { "params": { "g63Db": 1.5, "highCutHz": 9000.0, "mode": "graphic9" } } ],
      "enabled": true, "id": "eq", "perChannel": true, "type": "eq" },
    { "enabled": true, "id": "output", "params": { "levelDb": -4.0 }, "type": "output" }
  ],
  "format": "osp-preset", "id": "3b0e8c52-6f1a-4c3e-9b7d-2a5f0e1c0f13", "name": "Tone match", "schemaVersion": 1
})";

double valueOf(const Preset& p, const std::string& id)
{
    for (const auto& v : p.params)
        if (v.id == id)
            return v.value;
    FAIL("parameter not in the preset: " << id);
    return 0.0;
}

std::vector<float> defaults()
{
    std::vector<float> v;
    for (const auto& p : paramTable())
        v.push_back(p.def);
    return v;
}

void set(std::vector<float>& v, const std::string& id, float value)
{
    const int i = findParam(id);
    REQUIRE(i >= 0);
    v[static_cast<std::size_t>(i)] = value;
}

} // namespace

TEST_CASE("Preset: a tone-match document maps onto parameter ids", "[engine][preset]")
{
    std::string error;
    const auto p = parsePreset(kToneMatchLike, &error);
    REQUIRE(p.has_value());
    CHECK(p->id == kId);
    CHECK(p->name == "Tone match");
    CHECK(p->warnings.empty());
    CHECK(valueOf(*p, "input.gainDb") == 3.5);
    CHECK(valueOf(*p, "drive1.enabled") == 1.0);
    CHECK(valueOf(*p, "drive1.type") == 2.0); // "dist"
    // The crunch model is strip 2, so the amp, the cab and the EQ values land on channel 2 and it becomes active.
    CHECK(valueOf(*p, "amp.ch2.gain") == 7.25);
    CHECK(valueOf(*p, "amp.channel") == 1.0);
    CHECK(valueOf(*p, "cab.ch2.ir1.levelDb") == -2.0);
    CHECK(valueOf(*p, "eq.ch2.g63Db") == 1.5);
    CHECK(valueOf(*p, "eq.ch2.mode") == 0.0);
    CHECK(valueOf(*p, "eq.enabled") == 1.0);
    CHECK(valueOf(*p, "output.levelDb") == -4.0);
    REQUIRE(p->cabs.size() == 1);
    CHECK(p->cabs[0].channel == 1);
    CHECK(p->cabs[0].slot == 0);
    CHECK(p->cabs[0].uri == "factory:ir/412-a");
}

TEST_CASE("Preset: write -> parse -> write is byte-identical and keeps every value", "[engine][preset]")
{
    auto values = defaults();
    set(values, "amp.channel", 2.0f);
    set(values, "amp.ch3.gain", 8.3f);
    set(values, "delay.enabled", 1.0f);
    set(values, "delay.sync", 14.0f);
    set(values, "delay.timeMs", 333.3f);
    set(values, "reverb.mode", 3.0f);
    set(values, "eq.ch1.p2.q", 1.7f);
    set(values, "output.levelDb", -6.25f);
    const auto original = makePreset(kId, "My tone", values, {{2, 0, "factory:ir/412-b", ""}});

    const std::string first = writePreset(original);
    const auto back = parsePreset(first);
    REQUIRE(back.has_value());
    CHECK(back->name == "My tone");
    CHECK(back->warnings.empty());
    CHECK(writePreset(*back) == first);
    CHECK(presetValues(*back) == values); // exact: floats are written with enough digits to read back the same
    REQUIRE(back->cabs.size() == 1);
    CHECK(back->cabs[0].channel == 2);
    CHECK(back->cabs[0].uri == "factory:ir/412-b");
    CHECK(first.find("\"format\": \"osp-preset\"") != std::string::npos);
}

TEST_CASE("Preset: values outside the table are skipped with a warning, the rest still loads", "[engine][preset]")
{
    const auto p = parsePreset(R"({"format":"osp-preset","schemaVersion":1,"id":"x","name":"n","blocks":[
        {"type":"input","id":"input","params":{"gainDb":99,"nope":1}},
        {"type":"reverb","id":"reverb","params":{"mode":"cathedral","mixPct":"loud"}}]})");
    REQUIRE(p.has_value());
    CHECK(p->warnings.size() == 3);
    const auto values = presetValues(*p);
    CHECK(values[static_cast<std::size_t>(findParam("input.gainDb"))] == 24.0f); // clamped to the table range
    CHECK(values[static_cast<std::size_t>(findParam("reverb.mode"))] == 1.0f);   // default
}

TEST_CASE("Preset: rejects what is not a v1 preset", "[engine][preset]")
{
    std::string error;
    CHECK_FALSE(parsePreset("{", &error));
    CHECK(error == "malformed JSON");
    CHECK_FALSE(parsePreset(R"({"format":"osp-state"})", &error));
    CHECK_FALSE(parsePreset(R"({"format":"osp-preset","schemaVersion":2,"id":"a","name":"b","blocks":[]})", &error));
    CHECK(error.find("schemaVersion") != std::string::npos);
    CHECK_FALSE(parsePreset(R"({"format":"osp-preset","schemaVersion":1,"name":"b","blocks":[]})", &error));
    CHECK_FALSE(parsePreset(R"({"format":"osp-preset","schemaVersion":1,"id":"a","name":"b","blocks":7})", &error));
    CHECK_FALSE(parsePreset(std::string(20, '[') + std::string(20, ']'), &error));
    CHECK_FALSE(parsePreset(std::string(kMaxPresetBytes + 1, ' '), &error));
}

TEST_CASE("Preset: the plugin state carries every parameter and old blobs still read", "[engine][preset][state]")
{
    auto values = defaults();
    set(values, "amp.ch1.gain", 9.0f);
    set(values, "output.levelDb", -3.0f);
    PluginState s;
    s.presetId = kId;
    s.presetName = "Round trip";
    s.dirty = true;
    s.params = makePreset(kId, "Round trip", values).params;
    s.cabs = {{0, 0, "factory:ir/412-a", ""}};

    const auto blob = writeState(s);
    const auto back = readState(blob);
    REQUIRE(back.has_value());
    CHECK(back->outputLevelDb == -3.0);
    CHECK(back->cabs.size() == 1);
    Preset p;
    p.params = back->params;
    CHECK(presetValues(p) == values);
    CHECK(writeState(*back) == blob);

    PluginState legacy; // no params: the one-block blob of the pass-through engine
    legacy.presetId = kId;
    legacy.outputLevelDb = -9.0;
    const auto old = readState(writeState(legacy));
    REQUIRE(old.has_value());
    CHECK(old->outputLevelDb == -9.0);
    CHECK(old->params.empty()); // an old blob: consumers fall back to outputLevelDb
}
