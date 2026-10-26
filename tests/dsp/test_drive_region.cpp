#include "measure.hpp"
#include "osp/dsp/DriveRegion.h"
#include "rt_checker.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

using osp::dsp::DriveRegion;
using osp::dsp::DriveType;
using osp::dsp::OversamplingQuality;
namespace m = osp::measure;

namespace {

std::vector<float> noise(int n, std::uint32_t seed, float scale = 0.5f)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = scale * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// Block render under the RT checker; block size 0 adds a no-op call before every block.
template <typename PerBlock>
std::vector<float> render(DriveRegion& region, const std::vector<float>& in, int blockSize, PerBlock&& perBlock)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            perBlock(pos);
            if (blockSize == 0)
                region.process(in.data() + pos, out.data() + pos, 0);
            region.process(in.data() + pos, out.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}

std::vector<float> render(DriveRegion& region, const std::vector<float>& in, int blockSize = 64)
{
    return render(region, in, blockSize, [](int) {});
}

// Both slots enabled: drive1 mid (TS-style boost) into drive2 dist, the classic stacked pair.
void stack(DriveRegion& r)
{
    r.slot(0).setEnabled(true);
    r.slot(0).setType(DriveType::mid);
    r.slot(1).setEnabled(true);
    r.slot(1).setType(DriveType::dist);
    r.slot(1).setDrive(6.0f);
}

} // namespace

TEST_CASE("DriveRegion: OS-1 factor per rate and quality (ADR-0005 D2)", "[dsp][driveregion]")
{
    CHECK(DriveRegion::factorFor(44100.0, OversamplingQuality::live) == 4);
    CHECK(DriveRegion::factorFor(48000.0, OversamplingQuality::live) == 4);
    CHECK(DriveRegion::factorFor(88200.0, OversamplingQuality::live) == 2);
    CHECK(DriveRegion::factorFor(96000.0, OversamplingQuality::live) == 2);
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0})
        CHECK(DriveRegion::factorFor(fs, OversamplingQuality::hq) == 8);
}

TEST_CASE("DriveRegion: latency = round(1 kHz delay), constant across voicings and enables (REQ-OS-005, REQ-303)",
          "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto quality = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, quality == OversamplingQuality::hq);
    DriveRegion region; // both slots disabled: the transport alone, which is linear
    region.prepare(fs, 64, quality);

    std::vector<float> h(4096, 0.0f);
    h[0] = 1.0f;
    h = render(region, h);
    std::complex<double> num, den;
    const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const auto e = std::polar(static_cast<double>(h[n]), -w * static_cast<double>(n));
        num += static_cast<double>(n) * e;
        den += e;
    }
    const double d = (num / den).real();
    std::printf("OS-1 %dx @ %5.0f Hz: D = %.3f, latency %d, residual %+.3f\n", region.getFactor(), fs, d,
                region.getLatencySamples(), d - region.getLatencySamples());
    CHECK(std::abs(d - region.getMeasuredDelaySamples()) < 1e-3);
    CHECK(std::abs(d - region.getLatencySamples()) <= 0.5);
    if (quality == OversamplingQuality::live && fs == 48000.0)
        CHECK(region.getLatencySamples() <= 8); // half of the 16-sample Live budget (REQ-302): OS-1 + OS-2

    // Enabled slots at mix 0 are the same dry path through the same chain: bit-identical to both disabled.
    const auto in = noise(4096, 3u);
    region.reset();
    const auto disabled = render(region, in);
    DriveRegion dryMix;
    stack(dryMix);
    dryMix.slot(0).setMixPct(0.0f);
    dryMix.slot(1).setMixPct(0.0f);
    dryMix.prepare(fs, 64, quality);
    CHECK(dryMix.getLatencySamples() == region.getLatencySamples());
    CHECK(render(dryMix, in) == disabled);
}

TEST_CASE("DriveRegion: hard bypass nulls against the latency-aligned input (REQ-TEST-004)", "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    DriveRegion region;
    stack(region);
    region.setBypassed(true);
    region.prepare(fs, 64);
    const auto in = noise(4096, 8u);
    const auto out = render(region, in);
    CHECK(m::nullTest(out, in, static_cast<std::size_t>(region.getLatencySamples())).rmsDbfs <= -120.0);
}

TEST_CASE("DriveRegion: rate x block-size matrix, reset() and pre-prepare (REQ-TEST-001)", "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    const auto in = noise(4096, 13u);
    DriveRegion whole;
    stack(whole);
    whole.prepare(fs, 4096);
    const auto reference = render(whole, in, 4096);
    CHECK(osp::rt::test::scanOutput(reference).clean());
    whole.reset();
    CHECK(render(whole, in, 4096) == reference);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        DriveRegion chunked;
        stack(chunked);
        chunked.prepare(fs, block);
        CHECK(render(chunked, in, block) == reference);
    }
    DriveRegion unprepared;
    std::vector<float> out(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(in.data(), out.data(), 64); }));
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("DriveRegion: stacked drives meet the Live ASR gate at 48 kHz (REQ-OS-003)", "[dsp][driveregion][asr]")
{
    constexpr double fs = 48000.0;
    const double levelDbfs = GENERATE(-6.0, 0.0);
    const auto tone = m::coherentTone(1000.0, fs);
    DriveRegion region;
    stack(region);
    region.slot(1).setDrive(10.0f);
    region.prepare(fs, 64);
    const auto out = render(region, m::makeTone(tone, fs, levelDbfs, m::kWarmupSamples + m::kCoherentFftSize));
    const auto r = m::analyseCoherent(out, tone.bin);
    std::printf("OS-1 mid -> dist(10) 1 kHz %+.0f dBFS: ASR %.1f dB (gate -60)\n", levelDbfs, r.asrDb);
    CHECK(r.asrDb <= -60.0);
}

TEST_CASE("DriveRegion: random parameters on both slots and 100 quality toggles stay RT-clean (REQ-OS-006)",
          "[dsp][driveregion][rt]")
{
    const auto in = noise(64 * 200, 17u, 2.0f);
    std::uint32_t seed = 5u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    DriveRegion region;
    for (int k = 0; k < 100; ++k) {
        // Control thread: a Live/HQ toggle re-prepares (ADR-0005 D6). Audio thread: blocks with random parameters.
        region.prepare(48000.0, 64, k % 2 == 0 ? OversamplingQuality::live : OversamplingQuality::hq);
        const auto out = render(region, in, 64, [&](int) {
            for (int s = 0; s < DriveRegion::kNumSlots; ++s) {
                auto& slot = region.slot(s);
                slot.setEnabled(rnd() < 0.7f);
                slot.setType(static_cast<DriveType>(static_cast<int>(rnd() * 4.0f) % 4));
                slot.setDrive(rnd() * 10.0f);
                slot.setTone(rnd() * 10.0f);
                slot.setLevelDb(rnd() * 32.0f - 20.0f);
                slot.setLowCutHz(20.0f + rnd() * 280.0f);
                slot.setBright(rnd() < 0.5f);
                slot.setMixPct(rnd() * 100.0f);
            }
        });
        REQUIRE(osp::rt::test::scanOutput(out).clean());
    }
}
