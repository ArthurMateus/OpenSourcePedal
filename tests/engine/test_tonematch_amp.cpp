// Tone match amp search (ADR-0008 amendment A1, #197): render the bundled CC0 DI through a known classic amp setting,
// treat that as the reference and match from the same DI. The model must be recovered, and the match must land
// within a stated profile distance of the reference; the knob values need not be identical (several settings sound
// alike, and the cab/EQ stage absorbs part of the tone knobs).

#include "osp/dsp/conv/IrLoader.h"
#include "osp/engine/ToneMatch.h"

#include <catch2/catch_test_macros.hpp>

#include <dr_wav.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

std::vector<tmatch::CabIr> loadCabs(double fs, std::initializer_list<const char*> names)
{
    std::vector<tmatch::CabIr> cabs;
    for (const char* n : names) {
        unsigned rate = 0;
        const auto raw = readMono(std::string(OSP_ASSET_DIR) + "/cab/" + n + ".wav", rate);
        cabs.push_back({n, std::string("factory:ir/") + n, "",
                        osp::dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), rate, fs)});
    }
    return cabs;
}

float peakDb(const std::vector<float>& x)
{
    float p = 0.0f;
    for (float v : x)
        p = std::max(p, std::abs(v));
    return 20.0f * std::log10(p);
}

// Debug builds (the RT-checker CI job) run the renders about 20x slower, so they check one case on a short DI; the full
// matrix and the 30 s timing are Release checks. Define OSP_TONEMATCH_TEST_FAST to try the short form in Release.
#if defined(NDEBUG) && !defined(OSP_TONEMATCH_TEST_FAST)
constexpr bool kFast = false;
#else
constexpr bool kFast = true;
#endif

// Bounds of the amendment: the match's profile against the reference (robust spectral residual, flatness).
constexpr double kMaxResidualDb = 0.5;
constexpr double kMaxFlatnessDb = 0.5;
constexpr float kMaxGainError = 2.0f; // knob units

} // namespace

TEST_CASE("tonematch amps: recovers the model of a known amp setting", "[tonematch][slow]")
{
    unsigned rate = 0;
    auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    if (kFast)
        di.resize(std::min(di.size(), static_cast<std::size_t>(4.0 * rate)));
    const auto cabs = loadCabs(rate, {"112-a", "412-a", "412-c", "412-f"});

    struct Case {
        const char* name;
        tmatch::AmpKind model;
        float gain, bass, mid, treble;
        int cab;
    };
    const Case cases[] = {
        {"clean", tmatch::AmpKind::cleanA, 2.0f, 5.0f, 5.0f, 6.5f, 0},
        {"clean, bright voicing", tmatch::AmpKind::cleanA, 4.0f, 3.0f, 5.0f, 7.5f, 1},
        {"crunch", tmatch::AmpKind::crunchA, 5.0f, 6.0f, 4.0f, 6.0f, 2},
        {"lead", tmatch::AmpKind::leadA, 7.0f, 5.0f, 6.0f, 5.0f, 3},
    };
    tmatch::MatchOptions opt;
    opt.searchAmps = true;
    for (const auto& c : cases) {
        if (kFast && c.model != tmatch::AmpKind::crunchA)
            continue;
        DYNAMIC_SECTION(c.name)
        {
            tmatch::Setting hidden;
            hidden.inputGainDb = std::clamp(tmatch::kDiPeakDb - peakDb(di), -24.0f, 24.0f);
            hidden.amp = {c.model, c.gain, c.bass, c.mid, c.treble, 5.0f, 5.0f, 5.0f};
            hidden.cab = c.cab;
            const auto target = tmatch::analyse(tmatch::render(di, rate, hidden, cabs), rate);
            const auto r = tmatch::match(target, di, rate, cabs, opt);
            WARN(c.name << ": matched " << tmatch::toString(r.setting.amp.model) << " gain " << r.setting.amp.gain
                        << " (b/m/t " << r.setting.amp.bass << "/" << r.setting.amp.mid << "/" << r.setting.amp.treble
                        << ", p/d " << r.setting.amp.presence << "/" << r.setting.amp.depth << "), cab "
                        << cabs[static_cast<std::size_t>(r.setting.cab)].name << ", robust residual "
                        << r.robustResidualDb << " dB (unmatched " << r.unmatchedResidualDb << " dB), flatness "
                        << r.flatnessDistance << " dB");
            CHECK(r.setting.amp.model == c.model);
            CHECK(std::abs(r.setting.amp.gain - c.gain) <= kMaxGainError);
            CHECK(r.setting.cab == c.cab);
            CHECK(r.setting.drive == tmatch::DriveKind::clean); // the amp search leaves the pedal off
            CHECK(r.robustResidualDb < kMaxResidualDb);
            CHECK(r.flatnessDistance < kMaxFlatnessDb);
            CHECK(r.robustResidualDb < 0.5 * r.unmatchedResidualDb);

            const std::string json =
                tmatch::toPresetJson(r, cabs, "Amp round trip", "3b0e6a2c-8f41-4d7a-9c1e-5a2b7d9e0f13");
            CHECK(json.find(std::string("\"id\": \"") + tmatch::toString(c.model) + "\"") != std::string::npos);
            CHECK(json.find("\"type\": \"amp\"") != std::string::npos);
        }
    }
}

TEST_CASE("tonematch amps: a full match on a 30 s DI stays in minutes", "[tonematch][slow]")
{
    if (kFast)
        SKIP("the 30 s runtime is a Release measurement");
    unsigned rate = 0;
    auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const auto one = di;
    while (di.size() < static_cast<std::size_t>(30.0 * rate))
        di.insert(di.end(), one.begin(), one.end());
    di.resize(static_cast<std::size_t>(30.0 * rate));
    const auto cabs = loadCabs(rate, {"112-a", "412-a", "412-c", "412-f"});

    tmatch::Setting hidden;
    hidden.inputGainDb = std::clamp(tmatch::kDiPeakDb - peakDb(di), -24.0f, 24.0f);
    hidden.amp = {tmatch::AmpKind::crunchA, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f};
    hidden.cab = 1;
    const auto target = tmatch::analyse(tmatch::render(di, rate, hidden, cabs), rate);

    tmatch::MatchOptions opt;
    opt.searchAmps = true;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = tmatch::match(target, di, rate, cabs, opt);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("30 s DI, 4 cabs, 18 amp candidates + refinement: " << seconds << " s");
    CHECK(r.setting.amp.model == tmatch::AmpKind::crunchA);
    CHECK(seconds < 300.0);
}
