// Tone match app API (ADR-0008 A1, #197): buffers in, a preset out, progress, cancel, argument checks.

#include "osp/dsp/conv/IrLoader.h"
#include "osp/engine/ToneMatchApi.h"

#include <catch2/catch_test_macros.hpp>

#include <dr_wav.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace tmatch = osp::engine::tonematch;

namespace {

std::vector<float> readMono(const std::string& path, unsigned& rate)
{
    unsigned ch = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
    REQUIRE(raw != nullptr);
    std::vector<float> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i)
        x[i] = raw[i * ch];
    drwav_free(raw, nullptr);
    return x;
}

struct Fixture {
    unsigned rate = 0;
    std::vector<float> di;
    std::vector<tmatch::CabIr> cabs;
    Fixture()
    {
        di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
        // Debug builds (the RT-checker job) are about 20x slower: a shorter DI there.
#ifdef NDEBUG
        constexpr double kSeconds = 5.0;
#else
        constexpr double kSeconds = 3.0;
#endif
        di.resize(std::min(di.size(), static_cast<std::size_t>(kSeconds * rate)));
        for (const char* n : {"112-a", "412-c"}) {
            unsigned r = 0;
            const auto raw = readMono(std::string(OSP_ASSET_DIR) + "/cab/" + n + ".wav", r);
            cabs.push_back({n, std::string("factory:ir/") + n, "",
                            osp::dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), r, rate)});
        }
    }
    // The DI through a known amp setting, as a stereo "song" with the same signal on both sides.
    tmatch::ToneMatchRequest request() const
    {
        float peak = 0.0f;
        for (float v : di)
            peak = std::max(peak, std::abs(v));
        tmatch::Setting hidden;
        hidden.inputGainDb = tmatch::kDiPeakDb - 20.0f * std::log10(peak);
        hidden.amp.model = tmatch::AmpKind::crunchA;
        hidden.amp.gain = 5.0f;
        hidden.cab = 1;
        tmatch::ToneMatchRequest rq;
        rq.reference.left = tmatch::render(di, rate, hidden, cabs);
        rq.reference.right = rq.reference.left;
        rq.reference.sampleRate = rate;
        rq.di.left = di;
        rq.di.sampleRate = rate;
        rq.cabs = cabs;
        rq.presetName = "API test";
        rq.presetId = "3b0e6a2c-8f41-4d7a-9c1e-5a2b7d9e0f13";
        return rq;
    }
};

} // namespace

TEST_CASE("tonematch api: returns an amp preset and reports monotonic progress", "[tonematch][slow]")
{
    const Fixture f;
    const auto rq = f.request();
    std::vector<double> seen;
    const auto r = tmatch::runToneMatch(rq, [&](double fraction, const char*) {
        seen.push_back(fraction);
        return true;
    });
    REQUIRE_FALSE(r.cancelled);
    CHECK(r.match.setting.amp.model == tmatch::AmpKind::crunchA);
    CHECK(r.presetJson.find("\"id\": \"osp.amp.crunch_a\"") != std::string::npos);
    CHECK(r.presetJson.find("\"name\": \"API test\"") != std::string::npos);
    CHECK(r.presetJson.find(rq.presetId) != std::string::npos);
    REQUIRE(seen.size() > 10);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(seen.front() >= 0.0);
    CHECK(seen.back() == 1.0);
    WARN("API run on a short DI: " << r.seconds << " s, " << seen.size() << " progress calls");
}

TEST_CASE("tonematch api: the callback can cancel, without an exception", "[tonematch][slow]")
{
    const Fixture f;
    auto rq = f.request();
    int calls = 0;
    const auto r = tmatch::runToneMatch(rq, [&](double, const char*) { return ++calls < 6; });
    CHECK(r.cancelled);
    CHECK(r.presetJson.empty());
    CHECK(calls == 6); // stopped at the call that said no, and nothing ran after it
}

TEST_CASE("tonematch api: refuses bad input", "[tonematch]")
{
    const Fixture f;
    auto rq = f.request();
    auto noCabs = rq;
    noCabs.cabs.clear();
    CHECK_THROWS_AS(tmatch::runToneMatch(noCabs), std::invalid_argument);
    auto silent = rq;
    std::fill(silent.di.left.begin(), silent.di.left.end(), 0.0f);
    CHECK_THROWS_AS(tmatch::runToneMatch(silent), std::invalid_argument);
    auto empty = rq;
    empty.reference.left.clear();
    CHECK_THROWS_AS(tmatch::runToneMatch(empty), std::invalid_argument);
    auto badRange = rq;
    badRange.startSeconds = 100.0;
    CHECK_THROWS_AS(tmatch::runToneMatch(badRange), std::invalid_argument);
    auto badRate = rq;
    badRate.di.sampleRate = 0.0;
    CHECK_THROWS_AS(tmatch::runToneMatch(badRate), std::invalid_argument);
}
