// REQ-NAM-009 (ADR-0002 D1, ADR-0006 D3): no NAM core, Eigen or nlohmann include is reachable from any public
// header under engine/include/. The test follows every #include from those headers through our own public include
// directories (engine/include, dsp-core/include: osp_engine's PUBLIC interface), so a third-party include pulled in
// transitively is caught too. NAM core and Eigen are linked PRIVATE (cmake/OspDependencies.cmake), so their include
// directories never reach a consumer; this test pins the header side of that rule.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const fs::path kRoot = fs::path(OSP_ASSET_DIR).parent_path(); // OSP_ASSET_DIR = <repo>/assets
const std::vector<fs::path> kPublicIncludeDirs = {kRoot / "engine" / "include", kRoot / "dsp-core" / "include"};

struct Include {
    std::string name;
    bool quoted;
};

// Replaces // and /* */ comments with spaces, keeping newlines (a scanner, not std::regex: MSVC's regex recurses
// per character and has no multiline flag).
std::string stripComments(const std::string& text)
{
    std::string out = text;
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i] == '/' && out[i + 1] == '/') {
            for (; i < out.size() && out[i] != '\n'; ++i)
                out[i] = ' ';
        } else if (out[i] == '/' && out[i + 1] == '*') {
            const std::size_t end = out.find("*/", i + 2);
            const std::size_t stop = end == std::string::npos ? out.size() : end + 2;
            for (; i < stop; ++i)
                if (out[i] != '\n')
                    out[i] = ' ';
            --i;
        }
    }
    return out;
}

std::vector<Include> includesOf(const std::string& code)
{
    static const std::regex directive(R"(^\s*#\s*include\s*([<"])([^>"]+)[>"])");
    std::vector<Include> out;
    std::istringstream lines(code);
    std::smatch m;
    for (std::string line; std::getline(lines, line);)
        if (std::regex_search(line, m, directive))
            out.push_back({m[2].str(), m[1].str() == "\""});
    return out;
}

// Violations visible in one header's text, independent of resolution.
std::vector<std::string> textViolations(const std::string& text)
{
    static const std::regex badInclude(R"((^|/)(NAM/|Eigen/|unsupported/Eigen/|nlohmann/)|json\.hpp$)");
    static const std::regex badToken(R"((^|[^:\w])(Eigen|nlohmann|nam)::)");
    const std::string code = stripComments(text);
    std::vector<std::string> out;
    for (const Include& inc : includesOf(code))
        if (std::regex_search(inc.name, badInclude))
            out.push_back("#include " + inc.name);
    if (std::smatch m; std::regex_search(code, m, badToken))
        out.push_back("type " + m[2].str() + "::");
    return out;
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

fs::path resolve(const Include& inc, const fs::path& from)
{
    if (inc.quoted && fs::exists(from.parent_path() / inc.name))
        return fs::weakly_canonical(from.parent_path() / inc.name);
    for (const auto& dir : kPublicIncludeDirs)
        if (fs::exists(dir / inc.name))
            return fs::weakly_canonical(dir / inc.name);
    return {};
}

} // namespace

TEST_CASE("include isolation: the scanner flags NAM, Eigen and nlohmann includes and types", "[tools][nam]")
{
    CHECK(textViolations("#include <Eigen/Dense>\n").size() == 1);
    CHECK(textViolations("  #  include \"NAM/dsp.h\"\n").size() == 1);
    CHECK(textViolations("#include <nlohmann/json.hpp>\n").size() == 1);
    CHECK(textViolations("#include \"json.hpp\"\n").size() == 1);
    CHECK(textViolations("struct S { nlohmann::json j; };\n").size() == 1);
    CHECK(textViolations("using M = Eigen::MatrixXf;\n").size() == 1);
    CHECK(textViolations("std::unique_ptr<nam::DSP> p;\n").size() == 1);
    // Comments and our own namespaces are fine.
    CHECK(textViolations("// no NAM, no Eigen: nlohmann::json stays in src/\n#include <vector>\n").empty());
    CHECK(textViolations("/* #include <Eigen/Dense> */ osp::engine::nam::X* x;\n").empty());
}

TEST_CASE("include isolation: no NAM/Eigen/nlohmann include is reachable from engine/include (REQ-NAM-009)",
          "[tools][nam]")
{
    const fs::path engineInclude = kPublicIncludeDirs.front();
    REQUIRE(fs::is_directory(engineInclude));

    std::vector<fs::path> pending;
    for (const auto& e : fs::recursive_directory_iterator(engineInclude))
        if (e.is_regular_file() && (e.path().extension() == ".h" || e.path().extension() == ".hpp"))
            pending.push_back(fs::weakly_canonical(e.path()));
    const std::size_t roots = pending.size();
    REQUIRE(roots > 0);

    std::set<fs::path> visited;
    std::vector<std::string> violations;
    while (!pending.empty()) {
        const fs::path header = pending.back();
        pending.pop_back();
        if (!visited.insert(header).second)
            continue;
        const std::string text = readFile(header);
        for (const auto& v : textViolations(text))
            violations.push_back(header.generic_string() + ": " + v);
        for (const Include& inc : includesOf(stripComments(text))) {
            const fs::path target = resolve(inc, header);
            if (!target.empty())
                pending.push_back(target);
            else if (inc.quoted) // a quoted include must be one of ours; anything else is a private dependency
                violations.push_back(header.generic_string() + ": unresolved \"" + inc.name + "\"");
        }
    }
    INFO("public engine headers: " << roots << ", headers reached: " << visited.size());
    for (const auto& v : violations)
        UNSCOPED_INFO(v);
    CHECK(violations.empty());
    CHECK(visited.size() > roots); // the walk followed includes into dsp-core
}
