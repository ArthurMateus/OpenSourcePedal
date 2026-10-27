#include "osp/engine/State.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using osp::engine::PluginState;
using osp::engine::readState;
using osp::engine::writeState;

namespace {

PluginState sample()
{
    PluginState s;
    s.presetId = "3b0e8c52-6f1a-4c3e-9b7d-2a5f0e1c0f13";
    s.presetName = "Init";
    s.outputLevelDb = static_cast<double>(-3.2f); // values come from a float host parameter
    s.dirty = true;
    return s;
}

} // namespace

TEST_CASE("State: write -> read -> write is byte-identical", "[engine][state]")
{
    const auto first = writeState(sample());
    const auto parsed = readState(first);
    REQUIRE(parsed.has_value());
    CHECK(parsed->presetId == sample().presetId);
    CHECK(parsed->outputLevelDb == sample().outputLevelDb);
    CHECK(parsed->dirty);
    REQUIRE(writeState(*parsed) == first);
    REQUIRE(writeState(sample()) == first); // two writes of the same state are identical (REQ-902)
}

TEST_CASE("State: blob follows preset-schema.md section 8", "[engine][state]")
{
    const auto blob = writeState(sample());
    CHECK(blob.rfind(R"({"format":"osp-state",)", 0) == 0); // sorted keys: "format" < "preset" < "source" < ...
    CHECK(blob.find(R"("format":"osp-preset")") != std::string::npos);
    CHECK(blob.find(R"("schemaVersion":1)") != std::string::npos);
    CHECK(blob.find(R"("stateVersion":1)") != std::string::npos);
    CHECK(blob.find(R"("id":"output")") != std::string::npos);
}

TEST_CASE("State: rejects invalid blobs without throwing", "[engine][state]")
{
    CHECK_FALSE(readState("").has_value());
    CHECK_FALSE(readState("not json").has_value());
    CHECK_FALSE(readState(R"({"format":"osp-preset","schemaVersion":1})").has_value());
    CHECK_FALSE(readState(R"({"format":"osp-state","stateVersion":2,"preset":{}})").has_value());
    CHECK_FALSE(readState(R"({"format":"osp-state","stateVersion":1})").has_value()); // missing preset
    CHECK_FALSE(readState(R"({"format":"osp-state","stateVersion":1,"preset":{"format":"osp-preset",)"
                          R"("schemaVersion":1,"id":7,"name":"x","blocks":[]},"source":{}})")
                    .has_value()); // wrong type

    std::string deep(17, '[');
    deep += std::string(17, ']');
    CHECK_FALSE(readState(deep).has_value()); // nesting > 16
    CHECK_FALSE(readState(std::string((1u << 20) + 1, ' ')).has_value()); // > 1 MiB
}
