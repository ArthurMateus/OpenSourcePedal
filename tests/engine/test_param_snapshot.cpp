// REQ-PAR-001 (parameters.md §5): the host parameter list (ids, ranges, defaults) equals the checked-in snapshot
// tests/spec/parameters.json, and no id disappears. An intended change regenerates the snapshot in the same PR:
//   run osp_tests "[.update]" (hidden, it rewrites the file), check the diff, and commit tests/spec/parameters.json.

#include "osp/engine/Params.h"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

namespace {

using namespace osp::engine;

const char* const kSnapshot = OSP_SPEC_DIR "/parameters.json";

// Shortest text that reads back to the same float, so the snapshot is exact and stable across compilers.
std::string number(float v)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return {buf, r.ptr};
}

const char* name(ParamKind k)
{
    switch (k) {
    case ParamKind::boolean:
        return "bool";
    case ParamKind::choice:
        return "choice";
    case ParamKind::integer:
        return "int";
    case ParamKind::continuous:
        break;
    }
    return "float";
}

const char* name(Skew s)
{
    switch (s) {
    case Skew::log:
        return "log";
    case Skew::step:
        return "step";
    case Skew::lin:
        break;
    }
    return "lin";
}

// One host parameter per line, in table order (the host's parameter order), so a diff shows exactly what changed.
std::string snapshotJson()
{
    std::ostringstream out;
    out << "{\n  \"count\": " << hostParamCount() << ",\n  \"params\": [\n";
    bool first = true;
    for (const auto& p : paramTable()) {
        if (p.vis != Visibility::host)
            continue;
        out << (first ? "" : ",\n") << "    {\"id\": \"" << p.id << "\", \"kind\": \"" << name(p.kind)
            << "\", \"min\": " << number(p.min) << ", \"max\": " << number(p.max) << ", \"default\": " << number(p.def)
            << ", \"skew\": \"" << name(p.skew) << "\", \"unit\": \"" << p.unit << "\"";
        if (!p.choices.empty()) {
            out << ", \"choices\": [";
            for (std::size_t i = 0; i < p.choices.size(); ++i)
                out << (i ? ", " : "") << '"' << p.choices[i] << '"';
            out << ']';
        }
        out << '}';
        first = false;
    }
    out << "\n  ]\n}\n";
    return out.str();
}

std::string readFile(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The ids in a snapshot text: every `"id": "<id>"`.
std::set<std::string> ids(const std::string& json)
{
    std::set<std::string> out;
    const std::string key = "\"id\": \"";
    for (auto at = json.find(key); at != std::string::npos; at = json.find(key, at)) {
        at += key.size();
        const auto end = json.find('"', at);
        out.insert(json.substr(at, end - at));
    }
    return out;
}

} // namespace

TEST_CASE("Params: the host parameter list equals tests/spec/parameters.json (REQ-PAR-001)",
          "[engine][params][snapshot]")
{
    const auto actual = snapshotJson();
    const auto expected = readFile(kSnapshot);
    REQUIRE_FALSE(expected.empty());

    // Ids never disappear (checked first, so a removal is named even when the rest of the list changed too).
    const auto now = ids(actual);
    for (const auto& id : ids(expected)) {
        CAPTURE(id);
        CHECK(now.count(id) == 1);
    }
    CHECK(now.size() == 226);
    CHECK(actual == expected);
}

TEST_CASE("Params: regenerate tests/spec/parameters.json", "[.update]")
{
    std::ofstream(kSnapshot, std::ios::binary) << snapshotJson();
    WARN("rewrote " << kSnapshot);
}
