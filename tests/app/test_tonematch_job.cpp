// Issue #199: the tone-match run on a worker thread (progress, cancel, a preset that loads) and the yt-dlp arguments.

#include "AudioFiles.h"
#include "PluginProcessor.h"
#include "ToneMatchJob.h"
#include "YtDlp.h"

#include "osp/engine/Preset.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace osp::app;

namespace {

// The first `seconds` of the bundled DI clip as a mono 16-bit WAV in a temp folder, removed on exit.
struct ShortDi {
    fs::path dir, file;
    explicit ShortDi(double seconds)
        : dir(fs::temp_directory_path() / ("osp-tm-" + std::to_string(std::rand()))),
          file(dir / "di.wav")
    {
        fs::create_directories(dir);
        const auto src = decodeAudioFile(fs::path(OSP_SOURCE_ASSET_DIR) / "di" / "strum-arp-01.wav", seconds);
        REQUIRE(src.has_value());
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::FileOutputStream>(juce::File(juce::CharPointer_UTF16(file.c_str())));
        auto writer = wav.createWriterFor(
            stream,
            juce::AudioFormatWriterOptions{}.withSampleRate(src->sampleRate).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        const float* channel = src->left.data();
        REQUIRE(writer->writeFromFloatArrays(&channel, 1, static_cast<int>(src->left.size())));
    }
    ~ShortDi()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

#ifdef NDEBUG
constexpr double kSeconds = 5.0;
#else
constexpr double kSeconds = 3.0; // Debug builds are about 20x slower
#endif

ToneMatchSettings settingsFor(const ShortDi& di)
{
    ToneMatchSettings s;
    s.reference = di.file; // the DI as its own "song": the run is about the plumbing, not the match quality
    s.di = di.file;
    s.assetDir = OSP_SOURCE_ASSET_DIR;
    s.presetName = "Job test";
    return s;
}

} // namespace

TEST_CASE("Tone-match job: a run finishes with a preset the processor loads", "[app][tonematch][slow]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const ShortDi di(kSeconds);
    ToneMatchJob job;
    REQUIRE(job.start(settingsFor(di)));
    CHECK_FALSE(job.start(settingsFor(di))); // one run at a time

    double last = 0.0;
    bool monotonic = true;
    while (job.running()) {
        const double p = job.progress();
        monotonic = monotonic && p >= last;
        last = p;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    job.wait();
    const auto outcome = job.outcome();
    REQUIRE(job.state() == ToneMatchJob::State::finished);
    CHECK(outcome.error.empty());
    CHECK(monotonic);
    CHECK(job.progress() == 1.0);

    std::string why;
    const auto preset = osp::engine::parsePreset(outcome.presetJson, &why);
    REQUIRE(preset.has_value());
    CHECK(preset->name == "Job test");

    PluginProcessor p;
    p.prepareToPlay(48000.0, 256);
    CHECK(p.loadPreset(*preset).empty()); // its factory IRs resolve, its amp is a built-in one
    CHECK(p.presetName() == "Job test");
}

TEST_CASE("Tone-match job: cancel stops the run and leaves no preset", "[app][tonematch][slow]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const ShortDi di(kSeconds);
    ToneMatchJob job;
    REQUIRE(job.start(settingsFor(di)));
    while (job.running() && job.progress() < 0.05)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    job.cancel();
    job.wait();
    CHECK(job.state() == ToneMatchJob::State::cancelled);
    CHECK(job.outcome().presetJson.empty());
}

TEST_CASE("Tone-match job: a bad input fails with a reason instead of throwing", "[app][tonematch]")
{
    const juce::ScopedJuceInitialiser_GUI juce;
    const ShortDi di(1.0);
    auto s = settingsFor(di);
    s.reference = di.dir / "missing.wav";
    ToneMatchJob job;
    REQUIRE(job.start(s));
    job.wait();
    CHECK(job.state() == ToneMatchJob::State::failed);
    CHECK_FALSE(job.outcome().error.empty());

    s = settingsFor(di);
    s.assetDir = di.dir; // no cab folder
    REQUIRE(job.start(s)); // a finished job can be started again
    job.wait();
    CHECK(job.state() == ToneMatchJob::State::failed);
    CHECK(job.outcome().error.find("no cab IRs") != std::string::npos);
}

TEST_CASE("yt-dlp: only plain http(s) links are accepted and never read as options", "[app][tonematch]")
{
    using namespace osp::app::ytdlp;
    CHECK(isAcceptableUrl("https://www.youtube.com/watch?v=abc123"));
    CHECK(isAcceptableUrl("http://example.com/a"));
    for (const char* bad : {"", "youtube.com/watch", "ftp://example.com/a", "file:///C:/x.wav", "https://",
                            "https:///path", "--exec=calc", "https://a.com/x y", "https://a.com/\"q",
                            "https://a.com/'q", "https://a.com/`q`", "https://a.com/a\\b", "https://a.com/a\nb"})
        CHECK_FALSE(isAcceptableUrl(bad));
    CHECK(buildArgs("out", "--exec=calc").empty());
}

TEST_CASE("yt-dlp: the command line is audio only, WAV, one video, link last after --", "[app][tonematch]")
{
    using namespace osp::app::ytdlp;
    const std::string url = "https://www.youtube.com/watch?v=abc123&list=PL1";
    const auto args = buildArgs("work", url);
    REQUIRE(args.size() == 8);
    CHECK(args[0] == "--no-playlist");
    CHECK(args[1] == "-x");
    CHECK(args[3] == "wav");
    CHECK(args[6] == "--");
    CHECK(args[7] == url);
    CHECK(args[5].find("%(ext)s") != std::string::npos);
    CHECK(fs::path(args[5]).parent_path() == fs::path("work"));

    const auto file = outputFile("work", url);
    CHECK(file.extension() == ".wav");
    CHECK(file == outputFile("work", url)); // stable: the same link is fetched once
    CHECK(file != outputFile("work", url + "x"));
    CHECK(args[5].rfind(file.stem().string(), file.parent_path().string().size() + 1) != std::string::npos);
    CHECK_FALSE(installHint().empty());
}
