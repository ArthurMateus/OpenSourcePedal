#include "PresetJson.h"

#include "osp/engine/Params.h"
#include "osp/engine/Version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace osp::engine {

namespace {

using json = nlohmann::json;

constexpr int kStrips = 3; // amp channels (Graph::kNumChannels)
constexpr std::array<std::string_view, kStrips> kAmpModels{"osp.amp.clean_a", "osp.amp.crunch_a", "osp.amp.lead_a"};

int stripOfModel(std::string_view id)
{
    for (int i = 0; i < kStrips; ++i)
        if (kAmpModels[static_cast<std::size_t>(i)] == id)
            return i;
    return -1;
}

// The shortest decimal text that reads back as the same float, so a saved value restores exactly.
double shortest(float f)
{
    char buf[40];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, static_cast<double>(f));
        if (static_cast<float>(std::strtod(buf, nullptr)) == f)
            break;
    }
    return std::strtod(buf, nullptr);
}

json valueJson(const ParamInfo& p, double v)
{
    const float f = sanitize(p, static_cast<float>(v));
    switch (p.kind) {
    case ParamKind::boolean:
        return f > 0.5f;
    case ParamKind::choice:
        return std::string(p.choices[static_cast<std::size_t>(f)]);
    case ParamKind::integer:
        return static_cast<int>(f);
    case ParamKind::continuous:
        break;
    }
    return shortest(f);
}

// "drive1" -> "drive".
std::string blockType(const std::string& id)
{
    auto end = id.size();
    while (end > 1 && std::isdigit(static_cast<unsigned char>(id[end - 1])))
        --end;
    return id.substr(0, end);
}

struct Group {
    std::string id;
    bool enabled = true;
    int active = 0;
    json params = json::object();
    // {"params": {...}, "resources": {...}} per amp channel
    std::array<json, kStrips> channels{json{{"params", json::object()}}, json{{"params", json::object()}},
                                       json{{"params", json::object()}}};
    int usedChannels = 0;
};

} // namespace

int jsonNestingDepth(std::string_view text) noexcept
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

json presetToJson(const Preset& preset)
{
    std::vector<Group> groups;
    const auto group = [&](const std::string& id) -> Group& {
        for (auto& g : groups)
            if (g.id == id)
                return g;
        groups.push_back({});
        groups.back().id = id;
        return groups.back();
    };

    const auto table = paramTable();
    for (const auto& param : preset.params) {
        const int index = findParam(param.id);
        if (index < 0)
            continue;
        const ParamInfo& info = table[static_cast<std::size_t>(index)];
        const auto dot = param.id.find('.');
        Group& g = group(param.id.substr(0, dot));
        const std::string rest = param.id.substr(dot + 1);
        if (rest == "enabled") {
            g.enabled = param.value > 0.5;
        } else if (g.id == "amp" && rest == "channel") {
            g.active = static_cast<int>(sanitize(info, static_cast<float>(param.value)));
        } else if (rest.size() > 4 && rest.compare(0, 2, "ch") == 0 && rest[3] == '.' && rest[2] >= '1' &&
                   rest[2] <= '3') {
            const int ch = rest[2] - '1';
            g.channels[static_cast<std::size_t>(ch)]["params"][rest.substr(4)] = valueJson(info, param.value);
            g.usedChannels = std::max(g.usedChannels, ch + 1);
        } else {
            g.params[rest] = valueJson(info, param.value);
        }
    }
    for (const auto& cab : preset.cabs) {
        if (cab.channel < 0 || cab.channel >= kStrips || cab.slot < 0 || cab.slot > 1)
            continue;
        Group& g = group("cab");
        json ref = {{"uri", cab.uri}};
        if (!cab.sha256.empty())
            ref["sha256"] = cab.sha256;
        g.channels[static_cast<std::size_t>(cab.channel)]["resources"]["ir" + std::to_string(cab.slot + 1)] = ref;
        g.usedChannels = std::max(g.usedChannels, cab.channel + 1);
    }

    json blocks = json::array();
    for (auto& g : groups) {
        json block = {{"type", blockType(g.id)}, {"id", g.id}, {"enabled", g.enabled}};
        if (!g.params.empty())
            block["params"] = g.params;
        if (g.id == "amp") {
            block["activeChannel"] = g.active;
            json channels = json::array();
            for (int i = 0; i < kStrips; ++i) {
                json ch = g.channels[static_cast<std::size_t>(i)];
                ch["model"] = {{"kind", "builtin"}, {"id", std::string(kAmpModels[static_cast<std::size_t>(i)])}};
                channels.push_back(std::move(ch));
            }
            block["channels"] = std::move(channels);
        } else if (g.usedChannels > 0) {
            block["perChannel"] = true;
            json channels = json::array();
            for (int i = 0; i < g.usedChannels; ++i) {
                json ch = g.channels[static_cast<std::size_t>(i)];
                channels.push_back(std::move(ch));
            }
            block["channels"] = std::move(channels);
        }
        blocks.push_back(std::move(block));
    }

    json doc = {{"format", "osp-preset"}, {"schemaVersion", 1},          {"id", preset.id},
                {"name", preset.name},    {"blocks", std::move(blocks)}, {"createdWith", versionString()}};
    if (!preset.category.empty())
        doc["category"] = preset.category;
    if (!preset.author.empty())
        doc["author"] = preset.author;
    return doc;
}

std::string writePreset(const Preset& preset)
{
    return presetToJson(preset).dump(2) + "\n";
}

namespace {

void addValue(Preset& out, const std::string& id, const json& v)
{
    const int index = findParam(id);
    if (index < 0) {
        out.warnings.push_back("unknown parameter " + id);
        return;
    }
    const ParamInfo& info = paramTable()[static_cast<std::size_t>(index)];
    double value = 0.0;
    if (v.is_number()) {
        value = v.get<double>();
    } else if (v.is_boolean()) {
        value = v.get<bool>() ? 1.0 : 0.0;
    } else if (v.is_string() && info.kind == ParamKind::choice) {
        const auto name = v.get<std::string>();
        const auto it = std::find(info.choices.begin(), info.choices.end(), name);
        if (it == info.choices.end()) {
            out.warnings.push_back("unknown value \"" + name + "\" for " + id);
            return;
        }
        value = static_cast<double>(it - info.choices.begin());
    } else {
        out.warnings.push_back("wrong value type for " + id);
        return;
    }
    for (auto& p : out.params)
        if (p.id == id) {
            p.value = value;
            return;
        }
    out.params.push_back({id, value});
}

void addParams(Preset& out, const std::string& prefix, const json& params)
{
    if (!params.is_object())
        return;
    for (auto it = params.begin(); it != params.end(); ++it)
        addValue(out, prefix + it.key(), it.value());
}

} // namespace

std::optional<Preset> presetFromJson(const json& doc, std::string* error)
{
    const auto fail = [&](const char* why) -> std::optional<Preset> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (!doc.is_object() || doc.value("format", "") != "osp-preset")
        return fail("not an osp-preset document");
    if (doc.value("schemaVersion", 0) != 1)
        return fail("unsupported schemaVersion (this build reads 1)");
    if (!doc.contains("id") || !doc.at("id").is_string() || doc.at("id").get<std::string>().empty())
        return fail("missing id");
    if (!doc.contains("name") || !doc.at("name").is_string() || doc.at("name").get<std::string>().empty())
        return fail("missing name");
    if (!doc.contains("blocks") || !doc.at("blocks").is_array())
        return fail("missing blocks");

    Preset out;
    out.id = doc.at("id").get<std::string>();
    out.name = doc.at("name").get<std::string>();
    out.category = doc.value("category", "");
    out.author = doc.value("author", "");

    // The amp block fixes which strip each channel index means, so read it first.
    std::array<int, 8> stripOf{0, 1, 2, 3, 4, 5, 6, 7};
    for (const auto& block : doc.at("blocks")) {
        if (block.value("type", "") != "amp" || !block.contains("channels") || !block.at("channels").is_array())
            continue;
        int i = 0;
        for (const auto& ch : block.at("channels")) {
            if (i >= static_cast<int>(stripOf.size()))
                break;
            int strip = -1;
            if (ch.contains("model") && ch.at("model").is_object() && ch.at("model").value("kind", "") == "builtin")
                strip = stripOfModel(ch.at("model").value("id", ""));
            if (strip < 0)
                out.warnings.push_back("amp channel " + std::to_string(i + 1) + ": model not available, using strip " +
                                       std::to_string(i + 1));
            stripOf[static_cast<std::size_t>(i)] = strip >= 0 ? strip : i;
            ++i;
        }
    }

    for (const auto& block : doc.at("blocks")) {
        const std::string type = block.value("type", "");
        const std::string id = block.value("id", type);
        if (id.empty())
            continue;
        if (block.contains("enabled") && block.at("enabled").is_boolean() && findParam(id + ".enabled") >= 0)
            addValue(out, id + ".enabled", block.at("enabled"));
        if (block.contains("params"))
            addParams(out, id + ".", block.at("params"));

        const bool channelled = type == "amp" || block.value("perChannel", false);
        if (!channelled || !block.contains("channels") || !block.at("channels").is_array())
            continue;
        if (type == "amp") {
            const int active = std::clamp(block.value("activeChannel", 0), 0, 7);
            addValue(out, "amp.channel", json(stripOf[static_cast<std::size_t>(active)]));
        }
        int i = 0;
        for (const auto& ch : block.at("channels")) {
            if (i >= static_cast<int>(stripOf.size()) || stripOf[static_cast<std::size_t>(i)] >= kStrips) {
                out.warnings.push_back(id + ": channel " + std::to_string(i + 1) + " ignored (v1 has 3)");
                ++i;
                continue;
            }
            const int strip = stripOf[static_cast<std::size_t>(i)];
            addParams(out, id + ".ch" + std::to_string(strip + 1) + ".", ch.value("params", json::object()));
            if (ch.contains("resources") && ch.at("resources").is_object()) {
                for (int slot = 0; slot < 2; ++slot) {
                    const std::string key = "ir" + std::to_string(slot + 1);
                    if (type == "cab" && ch.at("resources").contains(key) && ch.at("resources").at(key).is_object()) {
                        const auto& ref = ch.at("resources").at(key);
                        out.cabs.push_back({strip, slot, ref.value("uri", ""), ref.value("sha256", "")});
                    }
                }
            }
            ++i;
        }
    }
    return out;
}

std::optional<Preset> parsePreset(std::string_view text, std::string* error) noexcept
{
    try {
        if (text.size() > kMaxPresetBytes || jsonNestingDepth(text) > 16) {
            if (error)
                *error = "document too large or nested too deeply";
            return std::nullopt;
        }
        const auto doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            if (error)
                *error = "malformed JSON";
            return std::nullopt;
        }
        return presetFromJson(doc, error);
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("unreadable preset: ") + e.what();
        return std::nullopt;
    }
}

std::vector<float> presetValues(const Preset& preset)
{
    const auto table = paramTable();
    std::vector<float> values(table.size());
    for (std::size_t i = 0; i < table.size(); ++i)
        values[i] = table[i].def;
    for (const auto& p : preset.params) {
        const int index = findParam(p.id);
        if (index >= 0)
            values[static_cast<std::size_t>(index)] =
                sanitize(table[static_cast<std::size_t>(index)], static_cast<float>(p.value));
    }
    return values;
}

Preset makePreset(std::string id, std::string name, std::span<const float> values, std::vector<PresetCab> cabs)
{
    Preset out;
    out.id = std::move(id);
    out.name = std::move(name);
    out.cabs = std::move(cabs);
    const auto table = paramTable();
    out.params.reserve(table.size());
    for (std::size_t i = 0; i < table.size(); ++i)
        out.params.push_back({table[i].id, static_cast<double>(i < values.size() ? values[i] : table[i].def)});
    return out;
}

} // namespace osp::engine
