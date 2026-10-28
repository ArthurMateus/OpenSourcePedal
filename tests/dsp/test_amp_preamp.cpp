// PreampStage (#171, ADR-0011 P5-5): curve vs the offline load line, small-signal response, THD vs drive, bias shift,
// smoothing, block/rate invariance, ADR-0005 D5 aliasing inside a 4x/2x test oversampler, fuzz, RT checker, latency.

#include "measure.hpp"
#include "osp/dsp/amp/PreampStage.h"
#include "osp/dsp/oversampling/Oversampler.h"
#include "rt_checker.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using osp::dsp::amp::PreampConfig;
using osp::dsp::amp::PreampStage;
namespace m = osp::measure;

namespace {

constexpr double kOsRate = 192000.0; // Live OS-2 at 48 kHz

std::vector<float> noise(int n, std::uint32_t seed, float scale = 0.5f)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = scale * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// Renders in blocks under the RT checker; perBlock(pos) may set parameters. Block size 0 adds a no-op call per block.
template <typename PerBlock>
std::vector<float> render(PreampStage& s, const std::vector<float>& in, int blockSize, PerBlock&& perBlock)
{
    std::vector<float> io = in;
    const int total = static_cast<int>(in.size()), step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            perBlock(pos);
            if (blockSize == 0)
                s.process(io.data() + pos, 0);
            s.process(io.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return io;
}
std::vector<float> render(PreampStage& s, const std::vector<float>& in, int blockSize = 256)
{
    return render(s, in, blockSize, [](int) {});
}

double gainDb(PreampStage& s, double fs, double hz, double levelDbfs = -60.0)
{
    const auto tone = m::coherentTone(hz, fs);
    s.reset();
    const auto out = render(s, m::makeTone(tone, fs, levelDbfs, m::kWarmupSamples * 4 + m::kCoherentFftSize));
    return m::analyseCoherent(out, tone.bin).fundamentalDbfs - levelDbfs;
}

// ADAA2 on a linear segment: (1 + z⁻¹ + z⁻²)/3, magnitude (1 + 2·cos ω)/3.
double adaa2Mag(double hz, double fs)
{
    return (1.0 + 2.0 * std::cos(2.0 * std::numbers::pi * hz / fs)) / 3.0;
}

double tptG(double hz, double fs)
{
    const double g = std::tan(std::numbers::pi * std::min(hz, 0.45 * fs) / fs);
    return g / (1.0 + g);
}

} // namespace

TEST_CASE("Preamp: the table follows the offline Koren load line (research Q2)", "[dsp][amp][preamp]")
{
    PreampStage s;
    s.prepare(kOsRate);
    // tools/amp/preamp_fit.py, same circuit and clamp (normalised: x = 1 is Vgk = 0, f'(0) = 1).
    const std::pair<double, double> ref[] = {{-3.0, -1.099169}, {-2.0, -1.082912}, {-1.5, -1.011353}, {-1.0, -0.807356},
                                             {-0.5, -0.458089}, {-0.1, -0.098504}, {0.1, 0.101411},   {0.5, 0.531040},
                                             {0.9, 0.986252},   {1.0, 1.078363},   {1.2, 1.108385},   {2.0, 1.134795},
                                             {4.0, 1.200754},   {16.0, 1.586876},  {64.0, 2.186021}};
    double worstTable = 0.0, worstTail = 0.0;
    for (const auto& [x, y] : ref)
        (x <= 4.0 ? worstTable : worstTail) = std::max(x <= 4.0 ? worstTable : worstTail, std::abs(s.curve(x) - y));
    std::printf("preamp curve vs load line: max error %.2e on [-3, 4], %.2e in the tail; circuit gain %.2f dB\n",
                worstTable, worstTail, s.circuitGainDb());
    CHECK(worstTable < 1.0e-4);
    CHECK(worstTail < 0.06); // the fitted tail: 0.057 (3.6 %) at x = 16
    CHECK(std::abs(s.circuitGainDb() - 35.65) < 0.01);
    // F1' = f and F2' = F1 everywhere, including across both table ends; f(0) = F1(0) = F2(0) = 0.
    CHECK(s.curve(0.0) == 0.0);
    CHECK(s.curveIntegral(0.0) == 0.0);
    CHECK(s.curveIntegral2(0.0) == 0.0);
    for (double x = -9.0; x <= 100.0; x += 0.37) {
        const double h = 1.0e-4;
        CHECK(std::abs((s.curveIntegral(x + h) - s.curveIntegral(x - h)) / (2.0 * h) - s.curve(x)) < 1.0e-6);
        CHECK(std::abs((s.curveIntegral2(x + h) - s.curveIntegral2(x - h)) / (2.0 * h) - s.curveIntegral(x)) < 1.0e-6);
    }
}

TEST_CASE("Preamp: small-signal gain, polarity and corners", "[dsp][amp][preamp]")
{
    PreampConfig cfg;
    cfg.outputGainDb = 20.0;
    PreampStage s;
    s.prepare(kOsRate, cfg);
    CHECK(std::abs(gainDb(s, kOsRate, 1000.0) - 20.0) < 0.02);
    // Polarity: a common-cathode stage inverts.
    const auto out = render(s, std::vector<float>(64, 1.0e-3f));
    CHECK(out[10] < 0.0f);
    // Coupling HPF and Miller LP at their corners (prewarped bilinear: exact at fc), plus ADAA2's roll-off.
    auto analog = [&](double hz, double hp, double lp) {
        const double a = hz / std::sqrt(hz * hz + hp * hp) / std::sqrt(1.0 + (hz / lp) * (hz / lp));
        return 20.0 * std::log10(a * adaa2Mag(hz, kOsRate)) + 20.0;
    };
    for (double hz : {12.0, 30.0, 100.0, 5000.0, 20000.0}) {
        const double f = m::coherentTone(hz, kOsRate).frequency;
        CAPTURE(hz, f);
        CHECK(std::abs(gainDb(s, kOsRate, hz) - analog(f, 12.0, 20000.0)) < 0.05);
    }
    s.setCouplingHz(150.0);
    s.setMillerHz(8000.0);
    for (double hz : {150.0, 8000.0}) {
        const double f = m::coherentTone(hz, kOsRate).frequency;
        CHECK(std::abs(gainDb(s, kOsRate, hz) - analog(f, 150.0, 8000.0)) < 0.05);
    }
    // Cathode shelf: −6 dB below 200 Hz, (s + g·ωp)/(s + ωp).
    cfg.cathodeShelfDb = -6.0;
    PreampStage shelf;
    shelf.prepare(kOsRate, cfg);
    shelf.setCouplingHz(1.0);
    for (double hz : {20.0, 200.0, 5000.0}) {
        const double f = m::coherentTone(hz, kOsRate).frequency, gl = std::pow(10.0, -6.0 / 20.0);
        const double expect =
            20.0 * std::log10(std::abs(std::complex<double>(gl * 200.0, f) / std::complex<double>(200.0, f)) *
                              adaa2Mag(f, kOsRate) / std::sqrt(1.0 + (f / 20000.0) * (f / 20000.0))) +
            20.0;
        CAPTURE(hz);
        CHECK(std::abs(gainDb(shelf, kOsRate, hz) - expect) < 0.05);
    }
}

TEST_CASE("Preamp: THD rises monotonically with drive and matches the load line", "[dsp][amp][preamp][thd]")
{
    PreampStage s;
    s.prepare(kOsRate);
    const auto tone = m::coherentTone(1000.0, kOsRate);
    const auto in =
        m::makeTone(tone, kOsRate, -20.0, m::kWarmupSamples * 4 + m::kCoherentFftSize); // x peak 0.1 at 0 dB
    double last = -1.0;
    for (int dB = -20; dB <= 40; dB += 4) {
        s.setDriveDb(static_cast<float>(dB));
        s.reset();
        const auto r = m::analyseCoherent(render(s, in), tone.bin);
        std::printf("preamp drive %+3d dB (peak x %6.2f): THD %6.2f %%\n", dB, 0.1 * std::pow(10.0, dB / 20.0),
                    100.0 * r.thd);
        CHECK(r.thd > last);
        last = r.thd;
        if (dB == 8) // peak 0.251: the offline curve gives 1.82 % (tools/amp/preamp_fit.py)
            CHECK(std::abs(r.thd / 0.0182 - 1.0) < 0.03);
    }
}

TEST_CASE("Preamp: DC is blocked, and the bias shift attacks fast and releases with the coupling RC",
          "[dsp][amp][preamp][dc]")
{
    PreampStage s;
    s.prepare(kOsRate);
    auto step = render(s, std::vector<float>(static_cast<std::size_t>(0.3 * kOsRate), 0.2f));
    CHECK(std::abs(step.back()) < 1.0e-4 * std::abs(step[0]));

    // 150 ms burst at x peak 4 (grid driven 4x past the onset), then silence.
    s.reset();
    const auto tone = m::coherentTone(1000.0, kOsRate);
    auto in = m::makeTone(tone, kOsRate, 12.04, static_cast<std::size_t>(0.6 * kOsRate));
    const auto burst = static_cast<std::size_t>(0.15 * kOsRate);
    std::fill(in.begin() + static_cast<long>(burst), in.end(), 0.0f);
    std::vector<double> bias(in.size());
    for (std::size_t n = 0; n < in.size(); ++n) {
        s.process(&in[n], 1);
        bias[n] = s.getBiasShift();
    }
    const double plateau = bias[burst - 1];
    const auto t63 =
        std::find_if(bias.begin(), bias.end(), [&](double b) { return b >= 0.632 * plateau; }) - bias.begin();
    const auto rel = std::find_if(bias.begin() + static_cast<long>(burst), bias.end(),
                                  [&](double b) { return b <= plateau / std::numbers::e; }) -
                     bias.begin() - static_cast<long>(burst);
    const double attackMs = 1e3 * static_cast<double>(t63) / kOsRate,
                 releaseMs = 1e3 * static_cast<double>(rel) / kOsRate;
    std::printf("preamp bias shift: plateau %.3f (grid units), 63 %% in %.2f ms, release to 1/e in %.2f ms\n", plateau,
                attackMs, releaseMs);
    CHECK(plateau > 1.0);
    CHECK(attackMs < 5.0);
    CHECK(std::abs(releaseMs - 1e3 / (2.0 * std::numbers::pi * 12.0)) < 0.5); // τ_r = 13.26 ms
    CHECK(std::abs(bias.back()) < 1.0e-6 * plateau); // recovered after 34 τ_r
    CHECK(std::abs(in.back()) < 1.0e-6f);
}

TEST_CASE("Preamp: parameter steps follow the smoothing laws per sample (no zipper, REQ-TEST-003)",
          "[dsp][amp][preamp][zipper]")
{
    constexpr int kStep = 48000 + 37, kDelivery = 48064; // mid-block step, delivered at the next 64-sample block
    PreampConfig cfg;
    cfg.cathodeShelfDb = -6.0;
    PreampStage s;
    s.prepare(kOsRate, cfg);
    const auto tone = m::coherentTone(700.0, kOsRate);
    const auto in = m::makeTone(tone, kOsRate, -12.0, 96000); // x peak 0.25 -> 1.0 (into the grid onset) after the step
    const auto out = render(s, in, 64, [&](int pos) {
        if (pos >= kStep) {
            s.setDriveDb(12.0f);
            s.setCouplingHz(120.0f);
            s.setMillerHz(9000.0f);
            s.setCathodeHz(400.0f);
        }
    });
    // Reference: the same algorithm in double, with each law applied per sample (parameters.md §1.4).
    auto law = [](double from, double to, double tau, long k) {
        return k < 0 ? from : to + (from - to) * std::pow(std::exp(-1.0 / (tau * kOsRate)), static_cast<double>(k + 1));
    };
    const double ratio = (68e3 + 1e6) / (68e3 + 2e3), off = ratio / (1.0 + ratio), cut = 1.0 - std::pow(10.0, -0.3);
    double cap = 0, mil = 0, cat = 0, x1 = 0, x2 = 0, F2x1 = 0, d12 = 0;
    std::vector<double> ref(in.size());
    for (std::size_t n = 0; n < in.size(); ++n) {
        const long k = static_cast<long>(n) - kDelivery;
        const double x = in[n] * law(1.0, std::pow(10.0, 0.6), 0.030, k);
        const double gc = law(tptG(12, kOsRate), tptG(120, kOsRate), 0.02, k);
        const double go = law(tptG(12 * (1 + ratio), kOsRate), tptG(120 * (1 + ratio), kOsRate), 0.02, k);
        double v = (x - cap) * gc, lp = v + cap;
        if (x - lp > 1.0) {
            v = (x - off - cap) * go;
            lp = v + cap;
        }
        cap = lp + v;
        v = (x - lp - mil) * law(tptG(20000, kOsRate), tptG(9000, kOsRate), 0.02, k);
        double u = v + mil;
        mil = u + v;
        v = (u - cat) * law(tptG(200, kOsRate), tptG(400, kOsRate), 0.02, k);
        const double low = v + cat;
        cat = low + v;
        u -= cut * low;
        const double tol = 1.0e-5 * (1.0 + std::abs(x1)), F2 = s.curveIntegral2(u);
        const double d01 = std::abs(u - x1) < tol ? s.curveIntegral(0.5 * (u + x1)) : (F2 - F2x1) / (u - x1);
        ref[n] = -(std::abs(u - x2) < tol ? s.curve((u + x1 + x2) / 3.0) : 2.0 * (d01 - d12) / (u - x2));
        x2 = x1;
        x1 = u;
        F2x1 = F2;
        d12 = d01;
    }
    const double r = m::residualDbc(out, ref);
    std::printf("preamp drive/corner step: residual %.1f dBc against the per-sample laws\n", r);
    CHECK(r <= -60.0);
}

TEST_CASE("Preamp: rate x block-size matrix, reset() determinism, pre-prepare (REQ-TEST-001)", "[dsp][amp][preamp]")
{
    const double base = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int factor = GENERATE(1, 2, 4);
    const double fs = base * factor;
    CAPTURE(fs);
    PreampConfig cfg;
    cfg.cathodeShelfDb = -8.0;
    cfg.outputGainDb = 6.0;
    const auto in = noise(8192, 7u, 0.9f);
    auto configure = [](PreampStage& s) { // targets move after prepare, so every smoother runs during the render
        s.setDriveDb(24.0f);
        s.setCouplingHz(90.0f);
        s.setMillerHz(9000.0f);
        s.setCathodeHz(300.0f);
    };
    PreampStage whole;
    whole.prepare(fs, cfg);
    configure(whole);
    const auto reference = render(whole, in, 8192);
    OSP_REQUIRE_OUTPUT_CLEAN(reference);
    whole.reset();
    const auto first = render(whole, in, 8192);
    whole.reset();
    CHECK(render(whole, in, 8192) == first);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        PreampStage chunked;
        chunked.prepare(fs, cfg);
        configure(chunked);
        CHECK(render(chunked, in, block) == reference);
    }
    PreampStage unprepared;
    std::vector<float> io(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(io.data(), 64); }));
    CHECK(std::all_of(io.begin(), io.end(), [](float v) { return v == 0.0f; }));
}

// D5 at 1, 2.5 and 5 kHz as the drive block and ADR-0005 A1 measure (at 8 kHz, H3 lands on 24 kHz, inside the
// decimator transition at 44.1/48 kHz: that measures the decimator, not the stage). At 88.2/96 kHz the Live factor is
// 2x: at max drive the full-band ASR misses D5 (1 kHz: H48 folding off the 48-50 kHz decimator transition, as in
// ADR-0005 A1; 2.5/5 kHz: internal folds into 20-48 kHz, gone at 4x). Every alias at or below 20 kHz still meets D5 by
// 10 dB or more, so those rows gate on the audible band and print the full-band value (ADR-0005 decision, #171 PR).
TEST_CASE("Preamp: ASR within ADR-0005 D5 Live gates in a test OS-2 (4x / 2x IIR)", "[dsp][amp][preamp][asr]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double f0 = GENERATE(1000.0, 2500.0, 5000.0);
    const float drive = GENERATE(0.0f, 40.0f);
    const double level = GENERATE(-6.0, 0.0);
    const int factor = fs < 60000.0 ? 4 : 2;
    CAPTURE(fs, f0, drive, level);
    const auto tone = m::coherentTone(f0, fs);
    const auto in = m::makeTone(tone, fs, level, m::kWarmupSamples + m::kCoherentFftSize);
    osp::dsp::Oversampler os;
    os.prepare(fs, 64, factor);
    PreampStage s;
    s.prepare(fs * factor);
    s.setDriveDb(drive);
    s.reset();
    std::vector<float> out(in.size());
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < in.size(); pos += 64)
            os.process(in.data() + pos, out.data() + pos, 64, [&](float* x, int n) { s.process(x, n); });
    }));
    const auto r = m::analyseCoherent(out, tone.bin);
    const double gate = f0 <= 2500.0 ? -60.0 : -40.0, asr = r.harmonicsBelowFloor ? r.aliasToFundamentalDb : r.asrDb;
    const auto p = m::powerSpectrum(std::vector<double>(out.end() - static_cast<long>(m::kCoherentFftSize), out.end()));
    double harmonics = 0.0, audible = 0.0;
    for (std::size_t b = 1; b < m::kCoherentFftSize / 2; ++b)
        if (b != tone.bin)
            (b % tone.bin == 0 ? harmonics : audible) +=
                b % tone.bin == 0 || b * fs / m::kCoherentFftSize <= 20000.0 ? p[b] : 0.0;
    const double asrAudible = m::powerToDb(audible / harmonics);
    std::printf("preamp ASR %4.0f Hz drive %+3.0f dB %+3.0f dBFS @ %5.0f x%d: %6.1f dB, aliases <= 20 kHz %6.1f dB "
                "(gate %.0f)\n",
                f0, static_cast<double>(drive), level, fs, factor, asr, asrAudible, gate);
    CHECK(asrAudible <= gate);
    if (factor == 4 || drive == 0.0f)
        CHECK(asr <= gate);
}

TEST_CASE("Preamp: parameter and input fuzz, NaN/Inf/subnormal scan, RT checker", "[dsp][amp][preamp][rt]")
{
    PreampConfig cfg;
    cfg.cathodeShelfDb = -10.0;
    PreampStage s;
    s.prepare(96000.0 * 2, cfg);
    std::uint32_t seed = 99u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN(), kInf = std::numeric_limits<float>::infinity();
    auto wild = [&](float lo, float hi) {
        const float r = rnd();
        return r < 0.03f ? kNan : r < 0.06f ? kInf : r < 0.09f ? -kInf : lo + (hi - lo) * (1.4f * rnd() - 0.2f);
    };
    auto in = noise(2048, 5u, 2.0f);
    in[100] = kNan;
    in[200] = kInf;
    in[300] = 1.0e30f;
    std::vector<float> io(in.size());
    for (int i = 0; i < 1000; ++i) {
        io = in;
        const int n = 1 + static_cast<int>(rnd() * 2047.0f);
        OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] {
            s.setDriveDb(wild(-40.0f, 40.0f));
            s.setCouplingHz(wild(1.0f, 500.0f));
            s.setMillerHz(wild(1000.0f, 24000.0f));
            s.setCathodeHz(wild(20.0f, 2000.0f));
            s.process(io.data(), n);
        }));
        OSP_REQUIRE_OUTPUT_CLEAN(std::span<const float>(io.data(), static_cast<std::size_t>(n)));
    }
    // Without FTZ/DAZ, a loud burst then silence decays to exact zero (states are flushed), never through subnormals.
    s.reset();
    std::vector<float> tail(static_cast<std::size_t>(192000), 0.0f);
    std::copy(in.begin(), in.begin() + 1000, tail.begin());
    tail[100] = tail[200] = tail[300] = 0.5f;
    s.process(tail.data(), static_cast<int>(tail.size()));
    OSP_REQUIRE_OUTPUT_CLEAN(tail);
    CHECK(tail.back() == 0.0f);
}

TEST_CASE("Preamp: reported delay matches the measured 1 kHz group delay", "[dsp][amp][preamp][latency]")
{
    const double fs = GENERATE(44100.0 * 4, 96000.0 * 2, 48000.0 * 8);
    PreampConfig cfg;
    cfg.cathodeShelfDb = GENERATE(0.0, -8.0);
    CAPTURE(fs, cfg.cathodeShelfDb);
    PreampStage s;
    s.prepare(fs, cfg);
    const auto a = m::coherentTone(1000.0, fs);
    const m::CoherentTone b{a.bin + 2, a.frequency * static_cast<double>(a.bin + 2) / static_cast<double>(a.bin)};
    auto in = m::makeTone(a, fs, -66.0, m::kWarmupSamples * 4 + m::kCoherentFftSize);
    const auto in2 = m::makeTone(b, fs, -66.0, in.size());
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] += in2[n];
    const auto out = render(s, in);
    auto spectrum = [](const std::vector<float>& x) {
        return m::realFft(std::vector<double>(x.end() - static_cast<long>(m::kCoherentFftSize), x.end()));
    };
    const auto X = spectrum(in), Y = spectrum(out);
    const auto ha = Y[a.bin] / X[a.bin], hb = Y[b.bin] / X[b.bin];
    const double measured = -std::arg(hb / ha) / (2.0 * std::numbers::pi * (b.frequency - a.frequency) / fs);
    const double reported = s.groupDelaySamples(0.5 * (a.frequency + b.frequency)); // same centre as the measurement
    std::printf("preamp delay @ %.0f Hz: 1 kHz %.4f; at %.1f Hz reported %.4f, measured %.4f samples\n", fs,
                s.getMeasuredDelaySamples(), 0.5 * (a.frequency + b.frequency), reported, measured);
    CHECK(std::abs(measured - reported) < 0.01);
    CHECK(s.getLatencySamples() == static_cast<int>(std::lround(s.getMeasuredDelaySamples())));
}

// Hidden (not in CI; bench entries belong to #83/#174): osp_tests "[.cpu]". One stage at 48 kHz x4 internal, 64-sample
// host blocks (256 stage samples per call), the drive smoother moving part of the time.
TEST_CASE("Preamp: CPU at 192 kHz in 256-sample blocks", "[.cpu]")
{
    PreampConfig cfg;
    cfg.cathodeShelfDb = -6.0;
    PreampStage s;
    s.prepare(kOsRate, cfg);
    auto buf = noise(256, 3u, 0.8f);
    constexpr int kBlocks = 30000; // 40 s of audio
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) {
        if (i % 750 == 0)
            s.setDriveDb((i / 750) % 2 ? 30.0f : 6.0f);
        s.process(buf.data(), 256);
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double audio = kBlocks * 256.0 / kOsRate;
    std::printf("preamp CPU: %.3f %% of one core (%.2f ns per stage sample; sink %g)\n", 100.0 * sec / audio,
                1e9 * sec / (kBlocks * 256.0), static_cast<double>(buf[7]));
    CHECK(sec > 0.0);
}
