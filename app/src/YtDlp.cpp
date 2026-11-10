#include "YtDlp.h"

#include <algorithm>
#include <cstdint>

namespace osp::app::ytdlp {

namespace {

// FNV-1a 64: a stable name for a link, not a security hash.
std::string keyFor(const std::string& url)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ull;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4)
        key[static_cast<std::size_t>(i)] = kHex[h & 0xf];
    return key;
}

} // namespace

bool isAcceptableUrl(const std::string& url)
{
    std::string rest;
    if (url.rfind("https://", 0) == 0)
        rest = url.substr(8);
    else if (url.rfind("http://", 0) == 0)
        rest = url.substr(7);
    else
        return false;
    if (rest.empty() || rest.front() == '/' || rest.front() == '?' || rest.front() == '#')
        return false; // no host
    return std::none_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '`' || c == '\\';
    });
}

std::filesystem::path outputFile(const std::filesystem::path& dir, const std::string& url)
{
    return dir / ("ref-" + keyFor(url) + ".wav");
}

std::vector<std::string> buildArgs(const std::filesystem::path& dir, const std::string& url)
{
    if (!isAcceptableUrl(url))
        return {};
    return {"--no-playlist",
            "-x",
            "--audio-format",
            "wav",
            "-o",
            (dir / ("ref-" + keyFor(url) + ".%(ext)s")).string(),
            "--",
            url};
}

std::string installHint()
{
    return "Links need yt-dlp and ffmpeg. Install them yourself: pip install yt-dlp "
           "(https://github.com/yt-dlp/yt-dlp), "
           "and ffmpeg from https://ffmpeg.org, then restart the app. Or download the song yourself and choose the "
           "file.";
}

} // namespace osp::app::ytdlp
