#pragma once

// The yt-dlp command line the tone-match panel runs to fetch a reference recording from a link (ADR-0008 A1). Nothing
// here starts a process or touches the network: it only validates the link and builds the arguments, so it can be
// tested. Same options as tools/tonematch/tonematch.py (audio only, WAV, no playlists).

#include <filesystem>
#include <string>
#include <vector>

namespace osp::app::ytdlp {

// http(s) with a host and no whitespace, control characters or quotes; and it does not start with '-', so it can never
// be read as an option. The panel only runs yt-dlp for a link that passes.
bool isAcceptableUrl(const std::string& url);

// `<dir>/ref-<key>.wav`: the file the run produces, `key` a stable 16-hex-digit hash of the link (the same link is
// fetched once).
std::filesystem::path outputFile(const std::filesystem::path& dir, const std::string& url);

// Arguments after the program name: --no-playlist -x --audio-format wav -o <dir>/ref-<key>.%(ext)s -- <url>. Empty
// for a link that is not acceptable. The `--` keeps the link from being taken for an option.
std::vector<std::string> buildArgs(const std::filesystem::path& dir, const std::string& url);

// What to tell the user when yt-dlp or ffmpeg is missing (the panel shows it instead of failing).
std::string installHint();

} // namespace osp::app::ytdlp
