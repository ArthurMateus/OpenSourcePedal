#include "PresetStore.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace osp::app {

namespace {

bool isPresetFile(const fs::path& p)
{
    auto name = p.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0;
}

void collect(const fs::path& dir, bool factory, std::vector<PresetEntry>& out, int* skipped)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return;
    std::vector<PresetEntry> found;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !isPresetFile(it->path()))
            continue;
        const auto preset = loadPresetFile(it->path());
        if (!preset) {
            if (skipped)
                ++*skipped;
            continue;
        }
        found.push_back({preset->name, preset->category, it->path(), factory});
    }
    std::sort(found.begin(), found.end(), [](const PresetEntry& a, const PresetEntry& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    out.insert(out.end(), found.begin(), found.end());
}

std::string readableError(const std::error_code& ec)
{
    return ec.message();
}

} // namespace

fs::path defaultUserPresetDir()
{
    std::string appData;
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "APPDATA") == 0 && buffer != nullptr)
        appData = buffer;
    std::free(buffer);
#else
    if (const char* value = std::getenv("APPDATA"))
        appData = value;
#endif
    if (appData.empty())
        return {};
    return fs::path(appData) / "OpenSourcePedal" / "Presets" / "User";
}

fs::path findFactoryPresetDir(const std::vector<fs::path>& candidates)
{
    std::error_code ec;
    for (const auto& c : candidates)
        if (!c.empty() && fs::is_directory(c, ec))
            return c;
    return {};
}

std::vector<PresetEntry> listPresets(const fs::path& factoryDir, const fs::path& userDir, int* skipped)
{
    if (skipped)
        *skipped = 0;
    std::vector<PresetEntry> out;
    collect(factoryDir, true, out, skipped);
    collect(userDir, false, out, skipped);
    return out;
}

std::optional<osp::engine::Preset> loadPresetFile(const fs::path& path, std::string* error)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        if (error)
            *error = "cannot read " + path.filename().string() + ": " + readableError(ec);
        return std::nullopt;
    }
    if (size > osp::engine::kMaxPresetBytes) {
        if (error)
            *error = path.filename().string() + " is larger than 1 MiB";
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        if (error)
            *error = "cannot read " + path.filename().string();
        return std::nullopt;
    }
    std::string why;
    auto preset = osp::engine::parsePreset(text, &why);
    if (!preset && error)
        *error = path.filename().string() + ": " + why;
    return preset;
}

std::string presetFileSlug(const std::string& name)
{
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80 && std::isalnum(u))
            out += static_cast<char>(std::tolower(u));
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (out.size() > 48)
        out.resize(48);
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? "preset" : out;
}

std::optional<fs::path> saveUserPreset(const fs::path& userDir, const osp::engine::Preset& preset, std::string* error)
{
    const auto fail = [&](const std::string& why) -> std::optional<fs::path> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (userDir.empty())
        return fail("no user preset folder (APPDATA is not set)");
    std::error_code ec;
    fs::create_directories(userDir, ec);
    if (ec)
        return fail("cannot create " + userDir.string() + ": " + readableError(ec));

    const std::string slug = presetFileSlug(preset.name);
    fs::path target;
    for (int n = 1; n < 1000; ++n) {
        target = userDir / (slug + (n == 1 ? "" : "-" + std::to_string(n)) + ".osp.json");
        if (!fs::exists(target, ec))
            break;
        const auto existing = loadPresetFile(target);
        if (existing && existing->id == preset.id)
            break; // the same preset: replace it
        if (n == 999)
            return fail("too many presets with the name " + preset.name);
    }

    const fs::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        const std::string text = osp::engine::writePreset(preset);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            fs::remove(temp, ec);
            return fail("cannot write " + temp.string());
        }
    }
    fs::rename(temp, target, ec);
    if (ec) { // rename onto an existing file can fail on Windows: replace it explicitly
        fs::remove(target, ec);
        ec.clear();
        fs::rename(temp, target, ec);
        if (ec) {
            fs::remove(temp, ec);
            return fail("cannot replace " + target.string() + ": " + readableError(ec));
        }
    }
    return target;
}

} // namespace osp::app
