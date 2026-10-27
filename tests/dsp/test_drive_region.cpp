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
using osp::dsp::DriveStage;
using osp::dsp::DriveType;
using osp::dsp::OversamplingFilter;
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
void stack(DriveStage& drive1, DriveStage& drive2)
{
    drive1.setEnabled(true);
    drive1.setType(DriveType::mid);
    drive2.setEnabled(true);
    drive2.setType(DriveType::dist);
    drive2.setDrive(6.0f);
}

void stack(DriveRegion& r)
{
    stack(r.slot(0), r.slot(1));
}

// Band-limited to 18 kHz: 64 tones at seeded random frequencies and phases, -12 dBFS RMS (as REQ-OS-001a).
std::vector<float> multitone(int n, double fs)
{
    std::uint32_t seed = 99u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    std::vector<double> x(static_cast<std::size_t>(n), 0.0);
    for (int k = 0; k < 64; ++k) {
        const double f = 20.0 + rnd() * (18000.0 - 20.0), ph = 2.0 * std::numbers::pi * rnd();
        for (int i = 0; i < n; ++i)
            x[static_cast<std::size_t>(i)] += std::sin(2.0 * std::numbers::pi * f / fs * i + ph);
    }
    const double scale = std::pow(10.0, -12.0 / 20.0) / std::sqrt(64.0 / 2.0);
    std::vector<float> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        out[i] = static_cast<float>(scale * x[i]);
    return out;
}

double maxStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t n = std::max<std::size_t>(from, 1); n < std::min(to, x.size()); ++n)
        worst = std::max(worst, static_cast<double>(std::abs(x[n] - x[n - 1])));
    return worst;
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
    if (quality == OversamplingQuality::hq) {
        // FIR + 6-sample pad: an exact integer, 80 (oversampler) + (2 slots + 6) / 8 (ADR-0005 D4, Amendment A2).
        CHECK(region.getFilter() == OversamplingFilter::fir);
        CHECK(region.getPadSamples() == 6);
        CHECK(std::abs(region.getMeasuredDelaySamples() - 81.0) < 1e-6);
        CHECK(region.getLatencySamples() == 81);
        CHECK(std::abs(d - 81.0) < 1e-3);
    } else {
        CHECK(region.getFilter() == OversamplingFilter::iir);
        CHECK(region.getPadSamples() == 0);
    }

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

TEST_CASE("DriveRegion: hard bypass is the exact latency-aligned input (REQ-TEST-004)", "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const auto quality = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, quality == OversamplingQuality::hq);
    DriveRegion region;
    stack(region);
    region.setBypassed(true);
    region.prepare(fs, 64, quality);
    const auto in = noise(4096, 8u);
    const auto out = render(region, in, 61);
    const auto latency = static_cast<std::ptrdiff_t>(region.getLatencySamples());
    std::vector<float> expected(in.size(), 0.0f);
    std::copy(in.begin(), in.end() - latency, expected.begin() + latency);
    CHECK(out == expected);
}

TEST_CASE("DriveRegion: HQ with both slots disabled is an exact integer delay, nulls at -120 dBFS (REQ-OS-001a)",
          "[dsp][driveregion][fir]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    constexpr int kLen = 16384;
    DriveRegion region;
    region.prepare(fs, 512, OversamplingQuality::hq);
    const auto in = multitone(kLen, fs);
    const auto out = render(region, in, 512);
    // Skip the start-up transient (one filter length) before comparing.
    constexpr std::size_t kSkip = 1024;
    const std::vector<float> outTail(out.begin() + kSkip, out.end()), inTail(in.begin() + kSkip, in.end());
    const auto null = m::nullTest(outTail, inTail, static_cast<std::size_t>(region.getLatencySamples()));
    std::printf("OS-1 HQ (FIR 8x + pad 6) @ %5.0f Hz: latency %d, null %.1f dBFS RMS\n", fs,
                region.getLatencySamples(), null.rmsDbfs);
    CHECK(null.rmsDbfs <= -120.0);
}

TEST_CASE("DriveRegion: rate x block-size matrix, reset() and pre-prepare (REQ-TEST-001)", "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto quality = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, quality == OversamplingQuality::hq);
    const auto in = noise(4096, 13u);
    DriveRegion whole;
    stack(whole);
    whole.prepare(fs, 4096, quality);
    const auto reference = render(whole, in, 4096);
    CHECK(osp::rt::test::scanOutput(reference).clean());
    whole.reset();
    CHECK(render(whole, in, 4096) == reference);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        DriveRegion chunked;
        stack(chunked);
        chunked.prepare(fs, block, quality);
        CHECK(render(chunked, in, block) == reference);
    }
    DriveRegion unprepared;
    std::vector<float> out(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(in.data(), out.data(), 64); }));
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("DriveRegion: stacked drives meet the Live ASR gate at 48 kHz, logged at 96 kHz/2x (REQ-OS-003)",
          "[dsp][driveregion][asr]")
{
    // REQ-OS-003 gates 44.1/48 kHz only. At 88.2/96 kHz the Live factor is 2x (ADR-0005 D2): logged, not gated.
    const double fs = GENERATE(48000.0, 96000.0);
    const double levelDbfs = GENERATE(-6.0, 0.0);
    const auto tone = m::coherentTone(1000.0, fs);
    DriveRegion region;
    stack(region);
    region.slot(1).setDrive(10.0f);
    region.prepare(fs, 64);
    const auto out = render(region, m::makeTone(tone, fs, levelDbfs, m::kWarmupSamples + m::kCoherentFftSize));
    const auto r = m::analyseCoherent(out, tone.bin);
    std::printf("OS-1 %dx @ %5.0f Hz, mid -> dist(10) 1 kHz %+.0f dBFS: ASR %.1f dB%s\n", region.getFactor(), fs,
                levelDbfs, r.asrDb, fs <= 48000.0 ? " (gate -60)" : " (logged)");
    if (fs <= 48000.0)
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

TEST_CASE("DriveRegion: HQ (FIR 8x) aliases no more than the IIR 8x path or Live (ADR-0005 D5, HQ logged)",
          "[dsp][driveregion][asr]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const double f0 = GENERATE(1000.0, 5000.0);
    const double levelDbfs = GENERATE(-6.0, 0.0);
    CAPTURE(fs, f0, levelDbfs);
    const auto tone = m::coherentTone(f0, fs);
    const auto in = m::makeTone(tone, fs, levelDbfs, m::kWarmupSamples + m::kCoherentFftSize);
    auto asr = [&](OversamplingQuality quality) {
        DriveRegion region;
        stack(region);
        region.slot(1).setDrive(10.0f);
        region.prepare(fs, 64, quality);
        return m::analyseCoherent(render(region, in), tone.bin).asrDb;
    };
    // The previous HQ path: the same two slots inside an 8x IIR oversampler.
    osp::dsp::Oversampler iir8;
    iir8.prepare(fs, 64, 8, OversamplingFilter::iir);
    DriveStage drive1, drive2;
    stack(drive1, drive2);
    drive2.setDrive(10.0f);
    drive1.prepare(fs * 8, 64 * 8);
    drive2.prepare(fs * 8, 64 * 8);
    std::vector<float> out(in.size());
    iir8.process(in.data(), out.data(), static_cast<int>(in.size()), [&](float* os, int osN) {
        drive1.process(os, osN);
        drive2.process(os, osN);
    });
    const double iir8Asr = m::analyseCoherent(out, tone.bin).asrDb;
    const double hq = asr(OversamplingQuality::hq), live = asr(OversamplingQuality::live);
    std::printf("OS-1 mid -> dist(10) %4.0f Hz %+.0f dBFS @ %5.0f Hz: ASR HQ %.1f, IIR 8x %.1f, Live %.1f dB "
                "(HQ target %d, warn only)\n",
                f0, levelDbfs, fs, hq, iir8Asr, live, f0 <= 2500.0 ? -80 : -60);
    CHECK(hq <= iir8Asr + 0.5);
    CHECK(hq <= live);
}

TEST_CASE("DriveRegion: a Live <-> HQ switch fades the new chain in from silence, without a step (REQ-OS-006)",
          "[dsp][driveregion]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const auto to = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, to == OversamplingQuality::hq);
    const auto from = to == OversamplingQuality::hq ? OversamplingQuality::live : OversamplingQuality::hq;
    // A full-scale 100 Hz cosine: it starts at its peak, the worst case for a step.
    std::vector<float> in(static_cast<std::size_t>(fs * 0.2));
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] = static_cast<float>(std::cos(2.0 * std::numbers::pi * 100.0 / fs * static_cast<double>(n)));
    DriveRegion region;
    region.prepare(fs, 64, from);
    render(region, in);
    region.prepare(fs, 64, to); // the control thread re-prepares (ADR-0005 D6)
    const auto out = render(region, in, 61);
    const auto fade = static_cast<std::size_t>(fs * DriveRegion::kSwitchFadeMs * 1e-3);
    CHECK(out[0] == 0.0f);
    const double steady = maxStep(out, out.size() / 2, out.size());
    const double onset = maxStep(out, 0, 4 * fade);
    std::printf("Switch to %s @ %5.0f Hz: onset step %.4f, steady step %.4f\n",
                to == OversamplingQuality::hq ? "HQ  " : "Live", fs, onset, steady);
    CHECK(onset <= 1.2 * steady);
    // The fade belongs to the switch: reset() replays it, and a first prepare() has none (so it would step here).
    region.reset();
    CHECK(render(region, in, 61) == out);
    DriveRegion fresh;
    fresh.prepare(fs, 64, to);
    CHECK(maxStep(render(fresh, in, 61), 0, 4 * fade) > 2.0 * steady);
}

TEST_CASE("DriveRegion: HQ fuzz, odd blocks, extreme input, NaN recovery after reset() (RT)", "[dsp][driveregion][rt]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    std::uint32_t seed = 11u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    DriveRegion region;
    region.prepare(fs, 127, OversamplingQuality::hq);
    auto in = noise(127 * 300, 21u, 1.5f);
    for (std::size_t n = 0; n < in.size(); n += 13)
        in[n] = (n % 2 ? 1.0f : -1.0f) * (n % 3 ? 1e-40f : 1e6f); // subnormal and huge samples
    const auto out = render(region, in, 127, [&](int) {
        for (int s = 0; s < DriveRegion::kNumSlots; ++s) {
            auto& slot = region.slot(s);
            slot.setEnabled(rnd() < 0.8f);
            slot.setType(static_cast<DriveType>(static_cast<int>(rnd() * 4.0f) % 4));
            slot.setDrive(rnd() * 12.0f - 1.0f); // includes out-of-range values, which clamp
            slot.setTone(rnd() * 12.0f - 1.0f);
            slot.setLevelDb(rnd() * 40.0f - 25.0f);
            slot.setLowCutHz(rnd() * 400.0f);
            slot.setBright(rnd() < 0.5f);
            slot.setMixPct(rnd() * 120.0f - 10.0f);
        }
    });
    CHECK(osp::rt::test::scanOutput(out).clean());

    // NaN input poisons the filter state until reset(), which then matches a freshly prepared region.
    DriveRegion fresh;
    stack(fresh);
    fresh.prepare(fs, 127, OversamplingQuality::hq);
    const auto clean = noise(4096, 4u);
    const auto expected = render(fresh, clean, 127);
    render(region, std::vector<float>(127, std::nanf("")), 127);
    stack(region);
    for (int s = 0; s < DriveRegion::kNumSlots; ++s) {
        region.slot(s).setTone(5.0f);
        region.slot(s).setLevelDb(0.0f);
        region.slot(s).setLowCutHz(20.0f);
        region.slot(s).setBright(false);
        region.slot(s).setMixPct(100.0f);
    }
    region.slot(0).setDrive(2.0f);
    region.reset();
    CHECK(render(region, clean, 127) == expected);
}
