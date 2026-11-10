#include "AudioFiles.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <memory>

namespace osp::app {

std::optional<DecodedAudio> decodeAudioFile(const std::filesystem::path& file, double maxSeconds, std::string* error)
{
    const auto fail = [error](std::string why) -> std::optional<DecodedAudio> {
        if (error)
            *error = std::move(why);
        return std::nullopt;
    };
    juce::AudioFormatManager formats;
    formats.registerBasicFormats(); // WAV, AIFF, FLAC, OGG and, with JUCE_USE_MP3AUDIOFORMAT, MP3
    const std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(juce::File(juce::CharPointer_UTF16(file.c_str()))));
    if (!reader || reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return fail("cannot read audio file: " + file.string());

    const auto length = static_cast<int>(
        std::min<juce::int64>(reader->lengthInSamples, static_cast<juce::int64>(reader->sampleRate * maxSeconds)));
    const int channels = static_cast<int>(std::min<unsigned>(reader->numChannels, 2));
    juce::AudioBuffer<float> buffer(channels, length);
    if (!reader->read(&buffer, 0, length, 0, true, channels > 1))
        return fail("audio read failed: " + file.string());

    DecodedAudio out;
    out.sampleRate = reader->sampleRate;
    out.left.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + length);
    if (channels > 1)
        out.right.assign(buffer.getReadPointer(1), buffer.getReadPointer(1) + length);
    return out;
}

} // namespace osp::app
