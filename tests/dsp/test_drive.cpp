#include "measure.hpp"
#include "osp/dsp/Drive.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "rt_checker.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <utility>
#include <vector>

using osp::dsp::DriveStage;
using osp::dsp::DriveType;
namespace m = osp::measure;

namespace {

constexpr DriveType kTypes[] = {DriveType::mid, DriveType::transparent, DriveType::dist, DriveType::fuzz};
constexpr const char* kNames[] = {"mid", "transparent", "dist", "fuzz"};

std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// Renders `in` through the stage at the stage's own rate, in blocks, under the RT checker. `perBlock(pos)` may set
// parameters before each block. Block size 0 adds a no-op call before every block.
template <typename PerBlock>
std::vector<float> render(DriveStage& stage, const std::vector<float>& in, int blockSize, PerBlock&& perBlock)
{
    std::vector<float> io = in;
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            perBlock(pos);
            if (blockSize == 0)
                stage.process(io.data() + pos, 0);
            stage.process(io.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return io;
}

std::vector<float> render(DriveStage& stage, const std::vector<float>& in, int blockSize = 64)
{
    return render(stage, in, blockSize, [](int) {});
}

// The stage inside a test-side 4x IIR region (the OS-1 setup of ADR-0005 D2 at 44.1/48 kHz).
std::vector<float> renderOversampled(DriveStage& stage, double fs, const std::vector<float>& in)
{
    osp::dsp::Oversampler os;
    os.prepare(fs, 64, 4);
    stage.prepare(fs * 4, 256);
    std::vector<float> out(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < in.size(); pos += 64)
            os.process(in.data() + pos, out.data() + pos, 64, [&](float* x, int n) { stage.process(x, n); });
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}

// LinearRamp law in double: the k-th sample after delivery (k >= 0) of a ramp from `from` to `to` over len samples.
double rampAt(double from, double to, int len, long k)
{
    return k >= len ? to : to - (to - from) / len * static_cast<double>(len - 1 - k);
}

} // namespace

TEST_CASE("Drive: ASR within ADR-0005 D5 Live gates at 4x IIR (REQ-OS-003)", "[dsp][drive][asr]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const int t = GENERATE(0, 1, 2, 3);
    const float drive = GENERATE(2.0f, 10.0f);
    const double levelDbfs = GENERATE(-6.0, 0.0);
    const double f0 = GENERATE(1000.0, 5000.0);
    CAPTURE(fs, kNames[t], drive, levelDbfs, f0);

    const auto tone = m::coherentTone(f0, fs);
    const auto in = m::makeTone(tone, fs, levelDbfs, m::kWarmupSamples + m::kCoherentFftSize);
    DriveStage stage;
    stage.setType(kTypes[t]);
    stage.setDrive(drive);
    const auto out = renderOversampled(stage, fs, in);
    const auto r = m::analyseCoherent(out, tone.bin);
    const double gate = f0 <= 2500.0 ? -60.0 : -40.0;
    const double asr = r.harmonicsBelowFloor ? r.aliasToFundamentalDb : r.asrDb;
    std::printf("drive %-11s drive %4.1f %5.0f Hz %+3.0f dBFS @ %5.0f: ASR %6.1f dB (gate %.0f)\n", kNames[t],
                static_cast<double>(drive), f0, levelDbfs, fs, asr, gate);
    CHECK(asr <= gate);
}

TEST_CASE("Drive: tight HPF corner and mid hump (small signal)", "[dsp][drive]")
{
    constexpr double fs = 48000.0;
    // Gain at f (dB) for a −60 dBFS sine, from the coherent analysis of the settled output.
    auto gainAt = [&](DriveType type, float lowCut, double f) {
        const auto tone = m::coherentTone(f, fs);
        DriveStage stage;
        stage.setType(type);
        stage.setDrive(0.0f);
        stage.setTone(20.0f / 3.0f); // shelf at 0 dB
        stage.setLowCutHz(lowCut);
        stage.prepare(fs, 64);
        const auto out = render(stage, m::makeTone(tone, fs, -60.0, m::kWarmupSamples + m::kCoherentFftSize));
        return m::analyseCoherent(out, tone.bin).fundamentalDbfs + 60.0;
    };
    // Transparent at drive 0 is linear for small signals: the 2nd-order Butterworth tight HPF is −3 dB at its corner.
    const double corner =
        gainAt(DriveType::transparent, 300.0f, 300.0) - gainAt(DriveType::transparent, 300.0f, 2000.0);
    CHECK(std::abs(corner + 3.0) < 0.3);
    // The TS-style voicing lifts 1 kHz well above 100 Hz (the 720 Hz pre-HPF on the gain leg, tone-targets-b F1).
    const double hump = gainAt(DriveType::mid, 20.0f, 1000.0) - gainAt(DriveType::mid, 20.0f, 100.0);
    std::printf("drive: tight corner %.2f dB, mid hump 1 kHz vs 100 Hz %.1f dB\n", corner, hump);
    CHECK(hump >= 9.0);
}

TEST_CASE("Drive: rate x block-size matrix, reset() and pre-prepare (REQ-TEST-001)", "[dsp][drive]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int t = GENERATE(0, 1, 2, 3);
    CAPTURE(fs, kNames[t]);
    const auto in = noise(4096, 11u);
    auto configure = [&](DriveStage& s) { // targets move after prepare, so the smoothers run during the render
        s.setType(kTypes[t]);
        s.setDrive(7.0f);
        s.setTone(2.0f);
        s.setLowCutHz(130.0f);
        s.setBright(true);
        s.setMixPct(70.0f);
        s.setLevelDb(-4.0f);
    };
    DriveStage whole;
    whole.prepare(fs, 4096);
    configure(whole);
    const auto reference = render(whole, in, 4096);
    REQUIRE(osp::rt::test::scanOutput(reference).clean());
    // reset() jumps the smoothers to their targets, so compare two renders that both start from reset().
    whole.reset();
    const auto first = render(whole, in, 4096);
    whole.reset();
    CHECK(render(whole, in, 4096) == first);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        DriveStage chunked;
        chunked.prepare(fs, block);
        configure(chunked);
        CHECK(render(chunked, in, block) == reference);
    }
    DriveStage unprepared;
    std::vector<float> io(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(io.data(), 64); }));
    CHECK(std::all_of(io.begin(), io.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Drive: level and mix steps follow the 20 ms ramp law (REQ-TEST-003)", "[dsp][drive][zipper]")
{
    constexpr double fs = 48000.0;
    constexpr int kLen = 960, kStep = 24000 + 37, kDelivery = 24064; // step mid-block, delivered at the next block
    const auto tone = m::coherentTone(1000.0, fs);
    const auto in = m::makeTone(tone, fs, -12.0, 48000);
    auto make = [&](float levelDb, float mix) {
        DriveStage s;
        s.setLevelDb(levelDb);
        s.setMixPct(mix);
        s.prepare(fs, 64);
        return s;
    };
    for (const auto [from, to] : {std::pair{-20.0f, 12.0f}, std::pair{12.0f, -20.0f}}) {
        DriveStage unity = make(0.0f, 100.0f), stepped = make(from, 100.0f);
        const auto wet = render(unity, in);
        const auto out = render(stepped, in, 64, [&](int pos) {
            if (pos >= kStep)
                stepped.setLevelDb(to);
        });
        const double g0 = std::pow(10.0, from / 20.0), g1 = std::pow(10.0, to / 20.0);
        std::vector<double> ref(in.size());
        for (std::size_t n = 0; n < in.size(); ++n)
            ref[n] = wet[n] *
                     (static_cast<long>(n) < kDelivery ? g0 : rampAt(g0, g1, kLen, static_cast<long>(n) - kDelivery));
        const double r = m::residualDbc(out, ref);
        std::printf("drive level %+.0f -> %+.0f dB: residual %.1f dBc\n", static_cast<double>(from),
                    static_cast<double>(to), r);
        CHECK(r <= -60.0);
    }
    DriveStage wetOnly = make(0.0f, 100.0f), dryOnly = make(0.0f, 0.0f);
    const auto wet = render(wetOnly, in), dry = render(dryOnly, in);
    for (const auto [from, to] : {std::pair{0.0f, 100.0f}, std::pair{100.0f, 0.0f}}) {
        DriveStage stepped = make(0.0f, from);
        const auto out = render(stepped, in, 64, [&](int pos) {
            if (pos >= kStep)
                stepped.setMixPct(to);
        });
        std::vector<double> ref(in.size());
        for (std::size_t n = 0; n < in.size(); ++n) {
            const double g = static_cast<long>(n) < kDelivery
                                 ? from / 100.0
                                 : rampAt(from / 100.0, to / 100.0, kLen, static_cast<long>(n) - kDelivery);
            ref[n] = dry[n] + g * (static_cast<double>(wet[n]) - dry[n]);
        }
        const double r = m::residualDbc(out, ref);
        std::printf("drive mix %.0f -> %.0f %%: residual %.1f dBc\n", static_cast<double>(from),
                    static_cast<double>(to), r);
        CHECK(r <= -60.0);
    }
}

TEST_CASE("Drive: hard bypass and disabled are the input delayed by 1 sample (REQ-TEST-004)", "[dsp][drive]")
{
    const auto in = noise(4096, 5u);
    DriveStage bypassed, disabled;
    bypassed.setBypassed(true);
    disabled.setEnabled(false);
    for (DriveStage* s : {&bypassed, &disabled}) {
        s->setType(DriveType::fuzz);
        s->setDrive(10.0f);
        s->prepare(192000.0, 64);
        const auto out = render(*s, in);
        CHECK(m::nullTest(out, in, static_cast<std::size_t>(DriveStage::kDelaySamples)).rmsDbfs <= -120.0);
    }
}

TEST_CASE("Drive: type changes cross-fade, queue, and settle on the last type", "[dsp][drive]")
{
    constexpr double fs = 192000.0;
    const auto in = m::makeTone(m::coherentTone(220.0, fs), fs, -12.0, 96000);
    DriveStage switched, direct;
    direct.setType(DriveType::dist);
    switched.prepare(fs, 64);
    direct.prepare(fs, 64);
    const auto out = render(switched, in, 64, [&](int pos) {
        if (pos == 9600)
            switched.setType(DriveType::fuzz); // fades 10 ms
        if (pos == 9600 + 640)
            switched.setType(DriveType::transparent); // queued behind the running fade
        if (pos == 9600 + 1280)
            switched.setType(DriveType::dist); // replaces the queued change
    });
    const auto ref = render(direct, in);
    CHECK(osp::rt::test::scanOutput(out).clean());
    CHECK(*std::max_element(out.begin(), out.end()) < 2.0f);
    // 200+ ms after the last fade the states have converged on the directly prepared dist stage.
    const std::vector<float> tail(out.end() - 9600, out.end()), refTail(ref.end() - 9600, ref.end());
    CHECK(m::residualDbc(tail, refTail) <= -60.0);
}

TEST_CASE("Drive: 1000 random parameter vectors, NaN/subnormal scan, NaN recovery (RT)", "[dsp][drive][rt]")
{
    const auto in = noise(64 * 1000, 9u);
    std::uint32_t seed = 1u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    DriveStage stage;
    stage.prepare(192000.0, 64);
    const auto out = render(stage, in, 64, [&](int) {
        stage.setType(kTypes[static_cast<int>(rnd() * 4.0f) % 4]);
        stage.setDrive(rnd() * 12.0f - 1.0f); // includes out-of-range values, which clamp
        stage.setTone(rnd() * 10.0f);
        stage.setLevelDb(rnd() * 40.0f - 25.0f);
        stage.setLowCutHz(rnd() * 400.0f);
        stage.setBright(rnd() < 0.5f);
        stage.setMixPct(rnd() * 100.0f);
        stage.setEnabled(rnd() < 0.8f);
    });
    CHECK(osp::rt::test::scanOutput(out).clean());

    // Subnormal and huge input stay clean; NaN poisons the state until reset(), which then matches a fresh stage.
    std::vector<float> extreme(4096);
    for (std::size_t n = 0; n < extreme.size(); ++n)
        extreme[n] = (n % 2 == 0 ? 1.0f : -1.0f) * (n < 2048 ? 1e-40f : 1e6f);
    CHECK(osp::rt::test::scanOutput(render(stage, extreme)).clean());
    DriveStage fresh;
    fresh.prepare(192000.0, 64);
    const auto clean = noise(2048, 4u);
    const auto expected = render(fresh, clean);
    render(stage, std::vector<float>(64, std::nanf("")));
    stage.setEnabled(true);
    stage.setType(DriveType::mid);
    stage.setDrive(2.0f);
    stage.setTone(5.0f);
    stage.setLevelDb(0.0f);
    stage.setLowCutHz(20.0f);
    stage.setBright(false);
    stage.setMixPct(100.0f);
    stage.reset();
    CHECK(render(stage, clean) == expected);
}

TEST_CASE("Drive: shape antiderivatives are consistent (F1' = f, F2' = F1)", "[dsp][drive]")
{
    auto check = [](auto shape) {
        using S = decltype(shape);
        constexpr double h = 1e-5;
        double worst1 = 0.0, worst2 = 0.0;
        for (double x = -3.0; x <= 3.0; x += 0.0137) {
            worst1 = std::max(worst1, std::abs((S::F1(x + h) - S::F1(x - h)) / (2 * h) - S::f(x)));
            worst2 = std::max(worst2, std::abs((S::F2(x + h) - S::F2(x - h)) / (2 * h) - S::F1(x)));
        }
        CHECK(worst1 < 1e-6);
        CHECK(worst2 < 1e-6);
    };
    check(osp::dsp::detail::CubicClipShape{});
    check(osp::dsp::detail::SoftLinearShape{});
    check(osp::dsp::detail::AsymClipShape{});
}

TEST_CASE("Drive: harmonic signature per voicing (THD, H2, H3; REQ-TEST-002)", "[dsp][drive][thd]")
{
    constexpr double fs = 48000.0;
    const int t = GENERATE(0, 1, 2, 3);
    const float drive = GENERATE(2.0f, 10.0f);
    CAPTURE(kNames[t], drive);
    const auto tone = m::coherentTone(220.0, fs);
    DriveStage stage;
    stage.setType(kTypes[t]);
    stage.setDrive(drive);
    const auto out =
        renderOversampled(stage, fs, m::makeTone(tone, fs, -12.0, m::kWarmupSamples + m::kCoherentFftSize));
    const std::vector<double> x(out.end() - static_cast<std::ptrdiff_t>(m::kCoherentFftSize), out.end());
    const auto p = m::powerSpectrum(x);
    const std::size_t k = tone.bin;
    const double h2 = m::powerToDb(p[2 * k] / p[k]), h3 = m::powerToDb(p[3 * k] / p[k]);
    const double thd = m::toDb(m::analyseCoherent(out, k).thd);
    std::printf("drive %-11s drive %4.1f: THD %6.1f dB, H2 %7.1f dBc, H3 %6.1f dBc\n", kNames[t],
                static_cast<double>(drive), thd, h2, h3);
    if (kTypes[t] == DriveType::fuzz)
        CHECK(h2 >= -40.0); // asymmetric: even harmonics at every drive
    else
        CHECK(h2 <= -100.0); // odd-symmetric shapes
}

TEST_CASE("Drive: the wet path is time-aligned with the dry path (phase delay at mix 0 vs 100 %)", "[dsp][drive]")
{
    // Transparent at drive 0 and −60 dBFS is linear, so the wet/dry transfer's phase delay is the voicing filters'
    // own: +0.2 samples at 2-4 kHz (48 kHz), mostly the 16 kHz post-LPF. Below that the 20 Hz tight HPF and the 10 Hz
    // DC blocker lead by about 1 sample at 500 Hz, so the check stays above 2 kHz. A dry path off by one sample moves
    // the delay by a full sample.
    constexpr double fs = 48000.0;
    for (double f : {2000.0, 4000.0}) {
        const auto tone = m::coherentTone(f, fs);
        const auto in = m::makeTone(tone, fs, -60.0, m::kWarmupSamples + m::kCoherentFftSize);
        auto binAt = [&](float mix) {
            DriveStage s;
            s.setType(DriveType::transparent);
            s.setDrive(0.0f);
            s.setTone(20.0f / 3.0f); // shelf at 0 dB
            s.setMixPct(mix);
            s.prepare(fs, 64);
            const auto out = render(s, in);
            const std::vector<double> x(out.end() - static_cast<std::ptrdiff_t>(m::kCoherentFftSize), out.end());
            return m::realFft(x)[tone.bin];
        };
        const double phaseDelay =
            -std::arg(binAt(100.0f) / binAt(0.0f)) / (2.0 * std::numbers::pi * tone.frequency / fs);
        std::printf("drive: wet vs dry phase delay at %4.0f Hz: %+.3f samples\n", f, phaseDelay);
        CHECK(std::abs(phaseDelay) < 0.5);
    }
}
