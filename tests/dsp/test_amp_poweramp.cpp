#include "measure.hpp"
#include "osp/dsp/amp/PowerAmp.h"
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
#include <vector>

using osp::dsp::amp::PowerAmp;
using osp::dsp::amp::PowerAmpShape;
using osp::dsp::amp::PowerAmpVoicing;
namespace m = osp::measure;

namespace {

constexpr double kOsRates[] = {176400.0, 192000.0, 384000.0, 768000.0}; // OS-2 rates (ADR-0005 D2)
const char* name(PowerAmpShape s)
{
    return s == PowerAmpShape::beam ? "beam" : "pentode";
}

std::vector<float> noise(std::size_t n, std::uint32_t seed, float scale)
{
    std::vector<float> x(n);
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = scale * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

std::vector<float> sine(double fs, double f, double amplitude, std::size_t n)
{
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / fs));
    return x;
}

// Block render under the RT checker; block 0 adds a 0-sample call before every 64-sample block.
template <typename PerBlock>
std::vector<float> render(PowerAmp& amp, std::vector<float> io, int block, PerBlock&& perBlock)
{
    const int total = static_cast<int>(io.size()), step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            perBlock(pos);
            if (block == 0)
                amp.process(io.data() + pos, 0);
            amp.process(io.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return io;
}

std::vector<float> render(PowerAmp& amp, std::vector<float> io, int block = 512)
{
    return render(amp, std::move(io), block, [](int) {});
}

PowerAmpVoicing voicing(PowerAmpShape shape, double nfbDb = 14.0, bool post = true)
{
    PowerAmpVoicing v;
    v.shape = shape;
    v.nfbDb = nfbDb;
    if (!post)
        v.otHpHz = v.otLpHz = 0.0;
    return v;
}

// Measured small-signal gain (dB) at f: Hann-weighted correlation of output and input after a settle time.
double gainDb(PowerAmp& amp, double fs, double f)
{
    const auto settle = static_cast<std::size_t>(fs * 0.6), len = static_cast<std::size_t>(fs * 0.4);
    const auto in = sine(fs, f, 1e-3, settle + len);
    const auto out = render(amp, in);
    std::complex<double> x, y;
    for (std::size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / len);
        const auto e = std::polar(w, -2.0 * std::numbers::pi * f * static_cast<double>(settle + i) / fs);
        x += static_cast<double>(in[settle + i]) * e;
        y += static_cast<double>(out[settle + i]) * e;
    }
    return 20.0 * std::log10(std::abs(y / x));
}

// Analytic phase margin (degrees) of the linearised loop: min over |T| = 1 crossings of 180° − |φ|, φ = arg T
// unwrapped from 0.01 Hz. gainMargin receives the smallest −20·log10|T| where |φ| crosses 180° (1e9 if never).
double phaseMargin(const PowerAmp& amp, double fs, bool unitDelay, double& gainMargin)
{
    auto t = [&](double lf) { return amp.loopGain(std::exp(lf), unitDelay); };
    const double lo = std::log(0.01), hi = std::log(0.5 * fs); // up to Nyquist, where z⁻¹ = −1
    constexpr int kSteps = 20000;
    double pm = 360.0, phase = std::arg(t(lo));
    gainMargin = 1e9;
    auto prev = t(lo);
    for (int i = 1; i <= kSteps; ++i) {
        const auto cur = t(lo + (hi - lo) * i / kSteps);
        const double prevPhase = phase;
        phase += std::arg(cur / prev); // unwrap: the step between neighbours is small
        if ((std::abs(prev) - 1.0) * (std::abs(cur) - 1.0) <= 0.0) { // linear interpolation on the dense grid
            const double a = (1.0 - std::abs(prev)) / (std::abs(cur) - std::abs(prev));
            pm = std::min(pm, 180.0 - std::abs(prevPhase + a * (phase - prevPhase)) * 180.0 / std::numbers::pi);
        }
        if ((std::abs(prevPhase) - std::numbers::pi) * (std::abs(phase) - std::numbers::pi) <= 0.0)
            gainMargin = std::min(gainMargin, -20.0 * std::log10(std::abs(cur)));
        prev = cur;
    }
    return pm;
}

} // namespace

TEST_CASE("PowerAmp: the analytic small-signal model matches the measured response", "[dsp][amp][poweramp]")
{
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    PowerAmp amp;
    amp.setVoicing(voicing(shape));
    amp.prepare(192000.0, 512);
    amp.setPresence(1.0f);
    amp.setDepth(8.0f);
    amp.setMaster(5.0f);
    amp.reset();
    for (double f : {30.0, 100.0, 1000.0, 3000.0, 10000.0, 20000.0}) {
        const double measured = gainDb(amp, 192000.0, f);
        const double model = 20.0 * std::log10(std::abs(amp.response(f)));
        CAPTURE(name(shape), f, measured, model);
        CHECK(std::abs(measured - model) < 0.05);
    }
}

TEST_CASE("PowerAmp: presence/depth reach +-10 dB at 0/10 and are flat at 5 (measured)", "[dsp][amp][poweramp]")
{
    const double fs = GENERATE(176400.0, 192000.0, 384000.0, 768000.0);
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    PowerAmp amp;
    amp.setVoicing(voicing(shape, 14.0, false)); // post filters off: the loop alone
    amp.prepare(fs, 512);
    auto at = [&](float presence, float depth, double f) {
        amp.setPresence(presence);
        amp.setDepth(depth);
        amp.reset();
        return gainDb(amp, fs, f);
    };
    const double ref = at(5.0f, 5.0f, 1000.0);
    for (double f : {20.0, 100.0, 3000.0, 20000.0}) {
        CAPTURE(fs, name(shape), f);
        // Flat at 5, apart from the ADAA1 half-sample average |cos(pi f/fs)| (-0.56 dB at 20 kHz / 176.4 kHz).
        const double adaa = 20.0 * std::log10(std::cos(std::numbers::pi * f / fs));
        CHECK(std::abs(at(5.0f, 5.0f, f) - ref - adaa) < 0.5);
    }
    const double hi = 20000.0, low = 15.0;
    const double p0 = at(0.0f, 5.0f, hi) - at(5.0f, 5.0f, hi), p10 = at(10.0f, 5.0f, hi) - at(5.0f, 5.0f, hi);
    const double d0 = at(5.0f, 0.0f, low) - at(5.0f, 5.0f, low), d10 = at(5.0f, 10.0f, low) - at(5.0f, 5.0f, low);
    std::printf(
        "poweramp %-7s @ %6.0f Hz: presence 0/10 at 20 kHz %+.2f/%+.2f dB, depth 0/10 at 15 Hz %+.2f/%+.2f dB\n",
        name(shape), fs, p0, p10, d0, d10);
    CHECK(std::abs(p0 + 10.0) <= 1.0);
    CHECK(std::abs(p10 - 10.0) <= 1.0);
    CHECK(std::abs(d0 + 10.0) <= 1.0);
    CHECK(std::abs(d10 - 10.0) <= 1.0);
}

TEST_CASE("PowerAmp: loop phase margin >= 45 deg over the knob grid; the unit-delay loop is not stable (analytic)",
          "[dsp][amp][poweramp]")
{
    // Analytic, from the coefficients (PowerAmp::loopGain); the model itself is checked against measurement above.
    // 360 means no |T| = 1 crossing. Master sits before the loop, so it does not change T.
    const double nfb = GENERATE(12.0, 14.0, 16.0, 20.0);
    double worst = 360.0, worstGm = 1e9, worstUnitGm = 1e9;
    for (double fs : kOsRates) {
        PowerAmp amp;
        amp.setVoicing(voicing(PowerAmpShape::beam, nfb));
        amp.prepare(fs, 64);
        for (int p = 0; p <= 4; ++p)
            for (int d = 0; d <= 4; ++d) {
                amp.setPresence(2.5f * p);
                amp.setDepth(2.5f * d);
                double gm = 0.0, gmUnit = 0.0;
                const double pm = phaseMargin(amp, fs, false, gm);
                worst = std::min(worst, pm);
                worstGm = std::min(worstGm, gm);
                phaseMargin(amp, fs, true, gmUnit);
                worstUnitGm = std::min(worstUnitGm, gmUnit);
                CAPTURE(nfb, fs, p, d, pm, gm);
                CHECK(pm >= 45.0);
                CHECK(gm >= 6.0);
            }
    }
    // The one-sample-delay loop keeps |T| > 1 up to Nyquist, so it fails by gain margin: |T| > 1 where arg T = -180
    // deg.
    std::printf("poweramp NFB %4.1f dB: worst phase margin %.1f deg, gain margin %s; one-sample-delay loop gain "
                "margin %.1f dB\n",
                nfb, worst, worstGm > 1e8 ? "inf (no -180 deg crossing)" : "finite", worstUnitGm);
    CHECK(worstUnitGm < 0.0);
}

TEST_CASE("PowerAmp: rate x block-size matrix, reset() determinism, pre-prepare (REQ-TEST-001)", "[dsp][amp][poweramp]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    CAPTURE(fs, name(shape));
    const auto in = noise(8192, 11u, 1.5f);
    auto v = voicing(shape);
    v.sagAmount = 0.3;
    auto configure = [&](PowerAmp& a) { // targets move after prepare, so the smoothers run during the render
        a.setPresence(8.0f);
        a.setDepth(1.5f);
        a.setMaster(9.0f);
    };
    PowerAmp whole;
    whole.setVoicing(v);
    whole.prepare(fs, 8192);
    configure(whole);
    const auto reference = render(whole, in, 8192);
    REQUIRE(osp::rt::test::scanOutput(reference).clean());
    whole.reset();
    const auto first = render(whole, in, 8192);
    whole.reset();
    CHECK(render(whole, in, 8192) == first);
    for (int block : {0, 1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        PowerAmp chunked;
        chunked.setVoicing(v);
        chunked.prepare(fs, block);
        configure(chunked);
        CHECK(render(chunked, in, block) == reference);
    }
    PowerAmp unprepared;
    std::vector<float> io(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(io.data(), 64); }));
    CHECK(std::all_of(io.begin(), io.end(), [](float x) { return x == 0.0f; }));
}

TEST_CASE("PowerAmp: parameter fuzz at max NFB stays finite, bounded and free of limit cycles (RT)",
          "[dsp][amp][poweramp][rt]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0, 768000.0);
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    CAPTURE(fs, name(shape));
    auto v = voicing(shape, 20.0);
    v.sagAmount = 0.5;
    PowerAmp amp;
    amp.setVoicing(v);
    amp.prepare(fs, 64);
    std::uint32_t seed = 3u;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    auto in = noise(64 * 1500, 5u, 4.0f);
    for (std::size_t i = 0; i < in.size(); ++i) // full-scale steps every 1000 samples on a third of the input
        if ((i / 1000) % 3 == 0)
            in[i] = (i / 1000) % 2 == 0 ? 1.0f : -1.0f;
    in.resize(in.size() + static_cast<std::size_t>(0.3 * fs), 0.0f); // then 300 ms of silence
    const std::size_t loud = 64 * 1500;
    const auto out = render(amp, in, 64, [&](int pos) {
        if (static_cast<std::size_t>(pos) >= loud)
            return;
        amp.setPresence(rnd() * 12.0f - 1.0f); // includes out-of-range values, which clamp
        amp.setDepth(rnd() < 0.05f ? std::nanf("") : rnd() * 10.0f);
        amp.setMaster(rnd() * 10.0f);
    });
    REQUIRE(osp::rt::test::scanOutput(out).clean());
    const std::size_t tailFrom = out.size() - static_cast<std::size_t>(0.01 * fs);
    float peak = 0.0f, tail = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
        (i < tailFrom ? peak : tail) = std::max(i < tailFrom ? peak : tail, std::abs(out[i]));
    std::printf("poweramp fuzz %-7s @ %6.0f Hz NFB 20 dB: peak %.3f, last 10 ms %.2e\n", name(shape), fs,
                static_cast<double>(peak), static_cast<double>(tail));
    // After 290 ms of silence only the slow DC recovery of the transformer pole is left: it is small, and it decays
    // (a limit cycle or oscillation would not).
    float late = 0.0f;
    for (std::size_t i = tailFrom - static_cast<std::size_t>(0.1 * fs);
         i < tailFrom - static_cast<std::size_t>(0.09 * fs); ++i)
        late = std::max(late, std::abs(out[i]));
    CHECK(peak <= 4.0f);
    CHECK(tail < 1e-4f);
    CHECK(tail < 0.7f * late);

    // Non-finite input samples are 0 and never poison the state.
    auto bad = noise(4096, 8u, 1.0f);
    auto zeroed = bad;
    for (std::size_t i = 0; i < bad.size(); i += 97) {
        bad[i] = i % 2 == 0 ? std::nanf("") : -std::numeric_limits<float>::infinity();
        zeroed[i] = 0.0f;
    }
    amp.reset();
    const auto a = render(amp, bad);
    amp.reset();
    CHECK(a == render(amp, zeroed));
}

TEST_CASE("PowerAmp: the tanh loop solve has converged at the default iteration count", "[dsp][amp][poweramp]")
{
    const float presence = GENERATE(0.0f, 5.0f);
    auto run = [&](int iterations) {
        auto v = voicing(PowerAmpShape::beam, 20.0);
        v.maxNewtonIterations = iterations;
        v.sagAmount = 0.3;
        PowerAmp amp;
        amp.setVoicing(v);
        amp.prepare(192000.0, 512);
        amp.setPresence(presence); // 0: the largest loop gain
        amp.setMaster(10.0f);
        amp.reset();
        return render(amp, noise(96256, 21u, 2.0f));
    };
    const auto a = run(PowerAmpVoicing{}.maxNewtonIterations), b = run(64);
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
    std::printf("poweramp beam presence %.0f: default vs 64 Newton steps, max difference %.2e\n",
                static_cast<double>(presence), worst);
    CHECK(worst <= 1e-6);
}

TEST_CASE("PowerAmp: THD rises monotonically with master (REQ-TEST-002)", "[dsp][amp][poweramp][thd]")
{
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    constexpr double fs = 192000.0;
    const auto tone = m::coherentTone(1000.0, fs);
    const auto in = m::makeTone(tone, fs, -6.0, m::kWarmupSamples + m::kCoherentFftSize);
    double prev = 0.0;
    for (int k = 0; k <= 10; ++k) {
        PowerAmp amp;
        amp.setVoicing(voicing(shape));
        amp.setMaster(static_cast<float>(k));
        amp.prepare(fs, 512);
        const double thd = m::analyseCoherent(render(amp, in), tone.bin).thd;
        std::printf("poweramp %-7s master %2d, 1 kHz -6 dBFS: THD %.4f %%\n", name(shape), k, 100.0 * thd);
        CAPTURE(k, thd, prev);
        CHECK(thd > prev);
        prev = thd;
    }
}

TEST_CASE("PowerAmp: sag lowers the headroom under sustained load and recovers with the release time",
          "[dsp][amp][poweramp][sag]")
{
    constexpr double fs = 192000.0;
    auto v = voicing(PowerAmpShape::pentode);
    v.sagAmount = 0.3;
    v.sagAttackMs = 20.0;
    v.sagReleaseMs = 200.0;
    PowerAmp amp;
    amp.setVoicing(v);
    amp.setMaster(10.0f);
    amp.prepare(fs, 512);
    auto peakOf = [](const std::vector<float>& x, std::size_t from, std::size_t to) {
        float p = 0.0f;
        for (std::size_t i = from; i < to; ++i)
            p = std::max(p, std::abs(x[i]));
        return static_cast<double>(p);
    };
    const auto burst = sine(fs, 1000.0, 1.0, static_cast<std::size_t>(0.5 * fs));
    const auto out = render(amp, burst);
    const double onset = peakOf(out, 0, 384), held = peakOf(out, out.size() - 1920, out.size());
    const double d0 = amp.getSupplyDroop();
    const double drop = 20.0 * std::log10(held / onset);
    render(amp, std::vector<float>(static_cast<std::size_t>(0.2 * fs), 0.0f)); // one release time
    const double d1 = amp.getSupplyDroop();
    render(amp, std::vector<float>(static_cast<std::size_t>(0.8 * fs), 0.0f)); // five in total
    const double d5 = amp.getSupplyDroop();
    std::printf("poweramp sag S 0.3: droop %.3f, peak %+.2f dB after 0.5 s; droop after 1/5 release times %.3f/%.4f\n",
                d0, drop, d1, d5);
    CHECK(d0 > 0.15);
    CHECK(drop < -1.5);
    CHECK(d1 <= std::exp(-1.0) * d0 * 1.02);
    CHECK(d5 <= 0.01 * d0);
    // After recovery, the next burst starts exactly as the first one.
    CHECK(std::abs(peakOf(render(amp, burst), 0, 384) - onset) <= 0.01 * onset);

    PowerAmp noSag; // default voicing: sagAmount 0 (patent note in PowerAmp.h)
    noSag.setMaster(10.0f);
    noSag.prepare(fs, 512);
    render(noSag, burst);
    CHECK(noSag.getSupplyDroop() == 0.0);
}

TEST_CASE("PowerAmp: ASR within ADR-0005 D5 Live gates at 4x IIR (REQ-OS-003)", "[dsp][amp][poweramp][asr]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const auto shape = GENERATE(PowerAmpShape::beam, PowerAmpShape::pentode);
    const float master = GENERATE(5.0f, 10.0f);
    const double levelDbfs = GENERATE(-6.0, 0.0);
    const double f0 = GENERATE(1000.0, 5000.0);
    const auto tone = m::coherentTone(f0, fs);
    // 1 s warm-up instead of kWarmupSamples: the transformer pole inside the loop settles with a ~0.7 Hz closed-loop
    // pole, and its onset transient would otherwise intermodulate into a skirt that the harness counts as aliases.
    const auto in = m::makeTone(tone, fs, levelDbfs, static_cast<std::size_t>(fs) + m::kCoherentFftSize);
    osp::dsp::Oversampler os;
    os.prepare(fs, 64, 4);
    PowerAmp amp;
    amp.setVoicing(voicing(shape));
    amp.setMaster(master);
    amp.prepare(fs * 4, 256);
    std::vector<float> out(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < in.size(); pos += 64)
            os.process(in.data() + pos, out.data() + pos, static_cast<int>(std::min<std::size_t>(64, in.size() - pos)),
                       [&](float* x, int n) { amp.process(x, n); });
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    const auto r = m::analyseCoherent(out, tone.bin);
    const double gate = f0 <= 2500.0 ? -60.0 : -40.0;
    const double asr = r.harmonicsBelowFloor ? r.aliasToFundamentalDb : r.asrDb;
    std::printf("poweramp %-7s master %4.1f %5.0f Hz %+3.0f dBFS @ %5.0f: THD %6.2f %%, ASR %6.1f dB (gate %.0f)\n",
                name(shape), static_cast<double>(master), f0, levelDbfs, fs, 100.0 * r.thd, asr, gate);
    CHECK(asr <= gate);
}

TEST_CASE("PowerAmp: a presence step is smoothed per sample (no zipper, block-size independent)",
          "[dsp][amp][poweramp][zipper]")
{
    constexpr double fs = 192000.0;
    const auto in = sine(fs, 8000.0, 0.05, static_cast<std::size_t>(0.3 * fs));
    const std::size_t stepAt = static_cast<std::size_t>(0.1 * fs) + 37;
    auto run = [&](int block) {
        PowerAmp amp;
        amp.setVoicing(voicing(PowerAmpShape::beam));
        amp.setPresence(0.0f);
        amp.prepare(fs, block);
        return render(amp, in, 1, [&](int pos) { // delivered per sample, so the step is not block-aligned
            if (static_cast<std::size_t>(pos) == stepAt)
                amp.setPresence(10.0f);
        });
    };
    const auto out = run(1);
    // Largest second difference |y[n] − 2y[n−1] + y[n−2]| during the glide vs. in the steady state after it: the
    // amplitude only rises, so a click or coefficient step would show as an excess over the final steady value.
    auto d2max = [&](std::size_t from, std::size_t to) {
        double p = 0.0;
        for (std::size_t i = from; i < to; ++i)
            p = std::max(p, static_cast<double>(std::abs(out[i] - 2.0f * out[i - 1] + out[i - 2])));
        return p;
    };
    const double glide = d2max(stepAt, out.size() - 9600), steady = d2max(out.size() - 9600, out.size());
    const double before = d2max(stepAt - 9600, stepAt);
    std::printf("poweramp presence 0 -> 10 step: max |d2y| before %.4f, glide %.4f, after %.4f\n", before, glide,
                steady);
    CHECK(glide <= 1.01 * steady);
    CHECK(steady > 2.5 * before); // the step did happen (about +20 dB at 8 kHz)
    const std::vector<float> window(out.begin() + static_cast<std::ptrdiff_t>(stepAt - 8192),
                                    out.begin() + static_cast<std::ptrdiff_t>(stepAt + 24576));
    std::printf("poweramp presence step splatter (informative) %.1f dBc\n", m::splatterDbc(window, fs, 8000.0));
}

TEST_CASE("PowerAmp: latency is 0 samples; the 1 kHz delay is the ADAA1 half sample", "[dsp][amp][poweramp]")
{
    for (double fs : kOsRates) {
        PowerAmp amp;
        amp.setVoicing(voicing(PowerAmpShape::beam, 14.0, false));
        amp.prepare(fs, 64);
        constexpr double df = 1.0;
        const double gd = -(std::arg(amp.response(1000.0 + df)) - std::arg(amp.response(1000.0 - df))) /
                          (2.0 * 2.0 * std::numbers::pi * df) * fs;
        std::printf("poweramp @ %6.0f Hz: 1 kHz group delay %.3f samples\n", fs, gd);
        CHECK(amp.getLatencySamples() == 0);
        CHECK(std::abs(gd - PowerAmp::kDelaySamples) < 0.1);
    }
}

TEST_CASE("PowerAmp: CPU per stage (local timing, not a bench entry)", "[.][perf][poweramp]")
{
    for (auto shape : {PowerAmpShape::beam, PowerAmpShape::pentode})
        for (double level : {0.05, 1.0}) {
            auto v = voicing(shape);
            v.sagAmount = 0.2;
            PowerAmp amp;
            amp.setVoicing(v);
            amp.setMaster(8.0f);
            amp.prepare(192000.0, 256);
            auto buf = sine(192000.0, 997.0, level, 192000);
            const auto t0 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 10; ++rep)
                for (std::size_t pos = 0; pos < buf.size(); pos += 256)
                    amp.process(buf.data() + pos, 256);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 10.0;
            std::printf("poweramp %-7s level %.2f: %.1f ns/sample at 192 kHz = %.2f %% of a core (48 kHz Live 4x)\n",
                        name(shape), level, s * 1e9 / 192000.0, 100.0 * s);
        }
}
