#pragma once

// Decodes a recording the user picked (WAV, AIFF, FLAC, OGG, MP3) for tone match. Worker or control thread: file I/O
// and allocation, never the audio thread.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace osp::app {

struct DecodedAudio {
    std::vector<float> left;
    std::vector<float> right; // empty for a mono file
    double sampleRate = 0.0;
    double seconds() const noexcept { return sampleRate > 0.0 ? static_cast<double>(left.size()) / sampleRate : 0.0; }
};

// Reads at most `maxSeconds` from the start (longer files are cut, not refused). nullopt, with the reason in *error,
// when the file cannot be opened or decoded or is empty. Of more than two channels the first two are used.
std::optional<DecodedAudio>
decodeAudioFile(const std::filesystem::path& file, double maxSeconds, std::string* error = nullptr);

} // namespace osp::app
