#include "CabIrFiles.h"

#include "Sha256.h" // tools/tonematch

#include <juce_audio_formats/juce_audio_formats.h>

#include <string_view>

namespace osp::app {

namespace fs = std::filesystem;

std::optional<fs::path> resolveCabFile(const osp::engine::PresetCab& cab, const fs::path& assetDir, std::string* error)
{
    const auto fail = [error](std::string why) -> std::optional<fs::path> {
        if (error)
            *error = std::move(why);
        return std::nullopt;
    };
    constexpr std::string_view kFactory = "factory:ir/";
    constexpr std::string_view kFile = "file:///";
    std::error_code ec;
    if (cab.uri.rfind(kFactory, 0) == 0) {
        const auto name = cab.uri.substr(kFactory.size());
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos)
            return fail("bad factory IR name: " + cab.uri);
        const auto path = assetDir / "cab" / (name + ".wav");
        if (assetDir.empty() || !fs::is_regular_file(path, ec))
            return fail("factory IR not found: " + name);
        return path;
    }
    if (cab.uri.rfind(kFile, 0) == 0) {
        const auto path = fs::path(
            juce::URL::removeEscapeChars(juce::String::fromUTF8(cab.uri.c_str() + kFile.size())).toWideCharPointer());
        if (!fs::is_regular_file(path, ec))
            return fail("IR file not found: " + path.string());
        if (!cab.sha256.empty()) {
            const auto hash = osp::tools::Sha256::ofFile(path.string());
            if (hash != cab.sha256)
                return fail("IR file changed (sha256 differs): " + path.string());
        }
        return path;
    }
    return fail("unsupported IR reference: " + cab.uri);
}

std::optional<DecodedIr> decodeIrFile(const fs::path& file, std::string* error)
{
    const auto fail = [error](std::string why) -> std::optional<DecodedIr> {
        if (error)
            *error = std::move(why);
        return std::nullopt;
    };
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(juce::CharPointer_UTF16(file.c_str()))));
    if (!reader || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return fail("cannot read IR: " + file.string());
    if (reader->sampleRate <= 0.0 || reader->lengthInSamples > static_cast<juce::int64>(reader->sampleRate * 10.0))
        return fail("IR is longer than 10 s: " + file.string());
    const auto length = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buffer(1, length);
    if (!reader->read(&buffer, 0, length, 0, true, false))
        return fail("IR read failed: " + file.string());
    DecodedIr out;
    out.sampleRate = reader->sampleRate;
    out.samples.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + length);
    return out;
}

} // namespace osp::app
