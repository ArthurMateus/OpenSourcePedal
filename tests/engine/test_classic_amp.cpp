// ClassicAmpModel (#174, ADR-0011 P5-8): level calibration, THD vs gain, ADR-0005 D5 aliasing at every rate (4x at
// 44.1/48 kHz, 2x at 88.2/96 kHz, full band), measured delay vs an impulse, rate x block matrix, reset determinism,
// bright/tight switch clicks, parameter-sweep zipper, fuzz and the RT checker.

#include "../../engine/src/amp/ClassicAmpModel.h" // the concrete type (setParameters is non-virtual, ADR-0011 D2)
#include "measure.hpp"
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
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

using osp::dsp::OversamplingQuality;
using osp::engine::AmpParams;
using osp::engine::ClassicAmpId;
using osp::engine::ClassicAmpModel;
namespace m = osp::measure;

namespace {

const char* name(ClassicAmpId id)
{
    return id == ClassicAmpId::cleanA ? "clean_a" : id == ClassicAmpId::crunchA ? "crunch_a" : "lead_a";
}

AmpParams defaults(ClassicAmpId id)
{
    AmpParams p;
    p.gain = id == ClassicAmpId::cleanA ? 3.0f : id == ClassicAmpId::crunchA ? 5.0f : 6.0f;
    return p;
}

// Renders in blocks under the RT checker; perBlock(pos, params) may change the parameters before each block.
template <typename PerBlock>
std::vector<float> render(ClassicAmpModel& amp, const std::vector<float>& in, int block, AmpParams p, PerBlock&& per)
{
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block) {
            per(pos, p);
            amp.setParameters(p);
            amp.process(in.data() + pos, out.data() + pos, std::min(block, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}
std::vector<float> render(ClassicAmpModel& amp, const std::vector<float>& in, int block, const AmpParams& p)
{
    return render(amp, in, block, p, [](int, AmpParams&) {});
}

std::unique_ptr<ClassicAmpModel>
prepared(ClassicAmpId id, double fs, int block, const AmpParams& p, OversamplingQuality q = OversamplingQuality::live)
{
    auto amp = std::make_unique<ClassicAmpModel>(id);
    amp->setParameters(p);
    amp->prepare(fs, block, q);
    return amp;
}

// Warm-up before the analysed window: 1 s at the host rate. The slowest poles (power-amp loop HPF 4 Hz, tau 40 ms;
// coupling RCs and the grid-conduction bias release) need about 15 tau to settle below the -120 dB floor; with
// the harness's 8192-sample default the unsettled transient read as -45...-58 dB "aliases" at max gain.
std::size_t settle(double fs)
{
    return std::max<std::size_t>(m::kWarmupSamples, static_cast<std::size_t>(fs));
}

double rmsDb(const std::vector<float>& x, std::size_t from)
{
    double s = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        s += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(s / static_cast<double>(x.size() - from));
}

// THD (%) of a coherent 1 kHz tone at `peakDbfs` through `amp` at 48 kHz.
double thdPercent(ClassicAmpId id, const AmpParams& p, double peakDbfs)
{
    auto amp = prepared(id, 48000.0, 512, p);
    const auto tone = m::coherentTone(1000.0, 48000.0);
    const auto out =
        render(*amp, m::makeTone(tone, 48000.0, peakDbfs, m::kWarmupSamples + m::kCoherentFftSize), 512, p);
    return 100.0 * m::analyseCoherent(out, tone.bin).thd;
}

} // namespace

TEST_CASE("ClassicAmp: ids, info, OS-2 factor per rate and quality", "[engine][amp][classic]")
{
    CHECK(osp::engine::classicAmpModelId(ClassicAmpId::cleanA) == "osp.amp.clean_a");
    CHECK(osp::engine::classicAmpModelId(ClassicAmpId::crunchA) == "osp.amp.crunch_a");
    CHECK(osp::engine::classicAmpModelId(ClassicAmpId::leadA) == "osp.amp.lead_a");
    const auto model = osp::engine::makeClassicAmp(ClassicAmpId::leadA);
    CHECK(model->info().architecture == "classic");
    for (const double fs : {44100.0, 48000.0, 88200.0, 96000.0}) {
        auto amp = prepared(ClassicAmpId::crunchA, fs, 64, {});
        CHECK(amp->getFactor() == (fs < 60000.0 ? 4 : 2));
        amp->prepare(fs, 64, OversamplingQuality::hq);
        CHECK(amp->getFactor() == 8);
        CHECK(amp->getLatencySamples() == static_cast<int>(std::ceil(amp->getMeasuredDelaySamples() - 1e-9)));
    }
}

// Research Q1: −18 dBFS RMS in at the defaults gives −18 dBFS RMS out (outputTrimDb); levelDb is tone-neutral.
TEST_CASE("ClassicAmp: level calibration at the defaults, levelDb gain", "[engine][amp][classic][level]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    auto p = defaults(id);
    const auto tone = m::coherentTone(1000.0, 48000.0);
    const auto in = m::makeTone(tone, 48000.0, -18.0 + 3.0103, 48000);
    auto amp = prepared(id, 48000.0, 256, p);
    const double out = rmsDb(render(*amp, in, 256, p), 24000);
    p.levelDb = -12.0f;
    amp->prepare(48000.0, 256, OversamplingQuality::live);
    const double quiet = rmsDb(render(*amp, in, 256, p), 24000);
    std::printf("level %-8s: -18.0 dBFS RMS in -> %6.2f dBFS RMS out; levelDb -12 -> %6.2f\n", name(id), out, quiet);
    CHECK(std::abs(out + 18.0) <= 1.0);
    CHECK(std::abs(quiet - out + 12.0) <= 0.01);
}

// THD at 1 kHz vs gain (0…10) at −12 dBFS peak: non-decreasing per model; research §5 targets at the default gain.
TEST_CASE("ClassicAmp: THD rises monotonically with gain; research section 5 targets", "[engine][amp][classic][thd]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    auto p = defaults(id);
    double last = 0.0;
    std::printf("THD %-8s at -12 dBFS, gain 0..10:", name(id));
    for (int g = 0; g <= 10; ++g) {
        p.gain = static_cast<float>(g);
        const double thd = thdPercent(id, p, -12.0);
        std::printf(" %.2f", thd);
        CHECK(thd >= last * 0.98); // non-decreasing, 2 % measurement slack
        last = std::max(last, thd);
    }
    p = defaults(id);
    const double at12 = thdPercent(id, p, -12.0), at0 = thdPercent(id, p, 0.0);
    std::printf("\n    defaults: %.2f %% at -12 dBFS, %.2f %% at 0 dBFS\n", at12, at0);
    if (id == ClassicAmpId::cleanA) {
        CHECK(at12 <= 1.0);
        CHECK(at0 <= 8.0);
    } else if (id == ClassicAmpId::crunchA) {
        CHECK(at12 >= 5.0);
        CHECK(at12 <= 10.0);
    } else {
        CHECK(at12 >= 30.0);
    }
}

// ADR-0005 D5 Live gates for each model at max gain (and at the defaults), with the ADR-0007 harness: full band, every
// alias in [1, N/2) counts (ADR-0005 A1 rejected a <= 20 kHz alias set). Live is 4x at 44.1/48 kHz and 2x at
// 88.2/96 kHz (ADR-0005 D2; #174 added the 2x rows). The audible-band ASR (aliases <= 20 kHz) is printed for context.
TEST_CASE("ClassicAmp: ASR within ADR-0005 D5 Live gates at every rate", "[engine][amp][classic][asr]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const double f0 = GENERATE(1000.0, 2500.0, 5000.0);
    const bool maxGain = GENERATE(false, true);
    const double level = GENERATE(-6.0, 0.0);
    CAPTURE(name(id), fs, f0, maxGain, level);
    auto p = defaults(id);
    if (maxGain)
        p.gain = p.master = 10.0f;
    const auto tone = m::coherentTone(f0, fs);
    auto amp = prepared(id, fs, 64, p);
    const auto out = render(*amp, m::makeTone(tone, fs, level, settle(fs) + m::kCoherentFftSize), 64, p);
    const auto r = m::analyseCoherent(out, tone.bin);
    const double gate = f0 <= 2500.0 ? -60.0 : -40.0, asr = r.harmonicsBelowFloor ? r.aliasToFundamentalDb : r.asrDb;
    const auto pw =
        m::powerSpectrum(std::vector<double>(out.end() - static_cast<long>(m::kCoherentFftSize), out.end()));
    double harmonics = 0.0, audible = 0.0;
    for (std::size_t b = 1; b < m::kCoherentFftSize / 2; ++b)
        if (b != tone.bin)
            (b % tone.bin == 0 ? harmonics : audible) +=
                b % tone.bin == 0 || b * fs / m::kCoherentFftSize <= 20000.0 ? pw[b] : 0.0;
    std::printf(
        "ASR %-8s %4.0f Hz %s %+3.0f dBFS @ %5.0f x%d: %6.1f dB (<= 20 kHz %6.1f, worst alias %6.1f dBc, gate %.0f)\n",
        name(id), f0, maxGain ? "max" : "def", level, fs, amp->getFactor(), asr, m::powerToDb(audible / harmonics),
        r.worstAliasDbc, gate);
    CHECK(asr <= gate);
}

// ADR-0011 D6 / REQ-301, with the orchestrator's decision on PR #191: the reported delay is the PROCESSING delay
// (OS-2 round trip + ADAA), the DriveRegion/NAM convention. Measured on the running model as the 1 kHz group delay
// from the phase of two coherent tones (bins k and k + 2) at -150 dBFS (small-signal: no stage reaches its grid onset
// even through lead_a's ~100 dB of gain), minus the modelled filters' group delay (getFilterGroupDelaySamples, from
// the stages' own small-signal analysis, each verified in the stage tests): within +-1 sample at every rate and
// quality. The reported value must also equal a separately prepared Oversampler's measured delay plus the ADAA terms.
// The full-model group delay is printed as a diagnostic only.
TEST_CASE("ClassicAmp: reported delay is the processing delay of the running model", "[engine][amp][classic][latency]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(name(id), fs, q == OversamplingQuality::hq);
    const auto p = defaults(id);
    auto amp = prepared(id, fs, 512, p, q);
    auto phase = [&](std::size_t bin) {
        const m::CoherentTone tone{bin, static_cast<double>(bin) * fs / m::kCoherentFftSize};
        const auto in = m::makeTone(tone, fs, -150.0, settle(fs) + m::kCoherentFftSize);
        amp->reset();
        const auto out = render(*amp, in, 512, p);
        auto tail = [](const std::vector<float>& x) {
            return std::vector<double>(x.end() - static_cast<long>(m::kCoherentFftSize), x.end());
        };
        return std::arg(m::realFft(tail(out))[bin] / m::realFft(tail(in))[bin]);
    };
    const std::size_t k = m::coherentTone(1000.0, fs).bin;
    const double dw = 2.0 * std::numbers::pi * 2.0 / m::kCoherentFftSize; // rad/sample between bins k and k + 2
    const double full = -std::remainder(phase(k + 2) - phase(k), 2.0 * std::numbers::pi) / dw;
    const double measured = full - amp->getFilterGroupDelaySamples();
    osp::dsp::Oversampler os;
    const int factor = amp->getFactor();
    os.prepare(fs, 512, factor,
               q == OversamplingQuality::hq ? osp::dsp::OversamplingFilter::fir : osp::dsp::OversamplingFilter::iir);
    const auto& v = amp->voicing();
    const double expected =
        os.getMeasuredDelaySamples() +
        (v.numStages * osp::dsp::amp::PreampStage::kDelaySamples + osp::dsp::amp::PowerAmp::kDelaySamples) / factor;
    std::printf("delay %-8s %5.0f Hz %s x%d: reported %8.3f, measured processing %8.3f, latency %3d "
                "(full-model 1 kHz group delay %8.3f, diagnostic)\n",
                name(id), fs, q == OversamplingQuality::hq ? "HQ  " : "Live", factor, amp->getMeasuredDelaySamples(),
                measured, amp->getLatencySamples(), full);
    CHECK(std::abs(measured - amp->getMeasuredDelaySamples()) <= 1.0);
    CHECK(std::abs(expected - amp->getMeasuredDelaySamples()) <= 1e-9);
    CHECK(amp->getLatencySamples() >= amp->getMeasuredDelaySamples());
    CHECK(amp->getLatencySamples() < amp->getMeasuredDelaySamples() + 1.0);
}

// REQ-TEST-001: every rate x block size (1, 32, 64, 127, 512, 2048) renders bit-identically; reset() makes a repeat
// render bit-identical; before prepare() the output is zero-filled; numSamples 0 is a no-op.
TEST_CASE("ClassicAmp: rate x block-size matrix, reset() determinism, pre-prepare", "[engine][amp][classic]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(name(id), fs);
    auto p = defaults(id);
    p.bright = true; // every path in use
    p.tight = true;
    std::vector<float> in(static_cast<std::size_t>(0.1 * fs));
    std::uint32_t seed = 7;
    for (std::size_t i = 0; i < in.size(); ++i) { // a decaying 220 Hz pluck plus noise, about -18 dBFS RMS
        seed = seed * 1664525u + 1013904223u;
        const double t = static_cast<double>(i) / fs;
        in[i] = static_cast<float>(0.3 * std::exp(-8.0 * t) * std::sin(2.0 * std::numbers::pi * 220.0 * t) +
                                   0.02 * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0));
    }
    auto reference = prepared(id, fs, 64, p);
    const auto ref = render(*reference, in, 64, p);
    OSP_REQUIRE_OUTPUT_CLEAN(ref);
    reference->reset();
    CHECK(render(*reference, in, 64, p) == ref);
    for (const int block : {1, 32, 127, 512, 2048}) {
        CAPTURE(block);
        auto amp = prepared(id, fs, block, p);
        CHECK(render(*amp, in, block, p) == ref);
    }
    ClassicAmpModel unprepared(id);
    std::vector<float> out(64, 1.0f);
    OSP_REQUIRE_RT_CLEAN(osp::rt::test::runAsAudioThread([&] { unprepared.process(in.data(), out.data(), 64); }));
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
    out.assign(64, 1.0f);
    reference->process(in.data(), out.data(), 0);
    CHECK(out[0] == 1.0f);
}

// Click metric for a parameter change at sample `at`, over [at − 50 ms, at + 100 ms] in 5 ms windows: `tilt` is the
// largest window ratio of 2nd-difference energy to signal energy (a click is broadband: it raises the HF share of a
// window), `envelope` the largest window RMS. Each is reported relative to the larger of the two static renders.
// A swept passive network may overshoot in level (envelope > 1) without clicking, and the specified laws' own ramp
// corners raise the tilt a little (levelDb: 36 dB in a 20 ms linear ramp), so the gate is tilt <= 1.25, or at most
// half of what the hard change (spliceTilt) gives.
struct ClickReport {
    double tilt = 0.0, envelope = 0.0;
    double spliceTilt = 0.0; // the same metric for a hard change: `before` up to `at`, `after` from it
    bool clean() const noexcept { return tilt <= 1.25 || tilt <= 0.5 * spliceTilt; }
};
ClickReport clicks(const std::vector<float>& changed,
                   const std::vector<float>& before,
                   const std::vector<float>& after,
                   int at,
                   double fs)
{
    const int from = at - static_cast<int>(0.05 * fs), to = at + static_cast<int>(0.1 * fs);
    const auto win = static_cast<int>(0.005 * fs);
    auto measure = [&](const std::vector<float>& x, double& tilt, double& env) {
        for (int w = from; w + win <= to; w += win / 4) {
            double hf = 0.0, e = 0.0;
            for (int i = w; i < w + win; ++i) {
                const auto u = static_cast<std::size_t>(i);
                const double d2 = static_cast<double>(x[u]) - 2.0 * x[u - 1] + x[u - 2];
                hf += d2 * d2;
                e += static_cast<double>(x[u]) * x[u];
            }
            tilt = std::max(tilt, hf / std::max(e, 1e-30));
            env = std::max(env, std::sqrt(e / win));
        }
    };
    double tc = 0.0, ec = 0.0, ts = 0.0, es = 0.0, tp = 0.0, ep = 0.0;
    measure(changed, tc, ec);
    measure(before, ts, es);
    measure(after, ts, es);
    auto splice = before;
    std::copy(after.begin() + at, after.end(), splice.begin() + at);
    measure(splice, tp, ep);
    return {tc / ts, ec / es, tp / ts};
}

// Research Q12: bright and tight crossfade over 10 ms with no click (ClickReport::clean).
// The residual against the two static renders mixed by the same ramp is printed: it is small where the switch acts
// ahead of mild nonlinearity, and large for lead_a's tight, where four clipping stages make a mix of outputs a poor
// reference (informative only).
TEST_CASE("ClassicAmp: bright and tight toggle without a click", "[engine][amp][classic][switch]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const bool tight = GENERATE(false, true);
    const bool on = GENERATE(true, false); // switch on, or off
    CAPTURE(name(id), tight, on);
    constexpr double fs = 48000.0;
    const auto in = m::makeTone(m::coherentTone(220.0, fs), fs, -12.0, 48000);
    constexpr int kBlock = 64, kToggle = 24000 + 37; // not block-aligned: it lands on the next block start
    auto with = [&](bool state) {
        auto p = defaults(id);
        (tight ? p.tight : p.bright) = state;
        return p;
    };
    auto a = prepared(id, fs, kBlock, with(!on));
    const auto before = render(*a, in, kBlock, with(!on));
    auto b = prepared(id, fs, kBlock, with(on));
    const auto after = render(*b, in, kBlock, with(on));
    auto t = prepared(id, fs, kBlock, with(!on));
    const auto toggled = render(*t, in, kBlock, with(!on), [&](int pos, AmpParams& p) {
        if (pos >= kToggle)
            p = with(on);
    });
    const int start = (kToggle + kBlock - 1) / kBlock * kBlock, len = static_cast<int>(std::lround(0.010 * fs));
    double err = 0.0, ref = 0.0;
    for (int i = start - 2400; i < start + 4800; ++i) {
        const auto u = static_cast<std::size_t>(i);
        const double w = std::clamp(static_cast<double>(i - start + 1) / len, 0.0, 1.0);
        const double mix = (1.0 - w) * before[u] + w * after[u];
        err += (toggled[u] - mix) * (toggled[u] - mix);
        ref += mix * mix;
    }
    const auto c = clicks(toggled, before, after, start, fs);
    std::printf("switch %-8s %s %s: HF tilt x%.3f (hard switch x%.3f), envelope x%.3f of static (residual vs ramped "
                "mix %6.1f dBc)\n",
                name(id), tight ? "tight " : "bright", on ? "on " : "off", c.tilt, c.spliceTilt, c.envelope,
                m::powerToDb(err / ref));
    CHECK(c.clean());
}

// REQ-TEST-003 at model level. (a) Sweep: each knob 0 -> 10 -> 0 over 0.5 s with new targets every 64 samples,
// rendered in 64-sample blocks and per sample (block 1, same target times): the stages smooth per sample, so the
// block size may not show (<= -60 dBc). (b) Step response: a full-range step (0 -> 10, levelDb -24 -> +12) on a
// block boundary leaves no click (ClickReport::clean). The smoothing laws themselves are
// gated per stage (#171-#173 tests).
TEST_CASE("ClassicAmp: knob sweeps and steps without zipper or click", "[engine][amp][classic][zipper]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const int knob = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(name(id), knob);
    constexpr double fs = 48000.0;
    const auto in = m::makeTone(m::coherentTone(330.0, fs), fs, -18.0, 24000);
    auto set = [&](AmpParams& p, double v) { // v in 0…10
        float* knobs[] = {&p.gain, &p.bass, &p.mid, &p.treble, &p.presence, &p.depth, &p.master, &p.levelDb};
        *knobs[knob] = static_cast<float>(knob == 7 ? -24.0 + 3.6 * v : v);
    };
    auto sweep = [&](int pos, AmpParams& p) {
        const int t = pos / 64 * 64;
        set(p, 10.0 * (1.0 - std::abs(2.0 * t / static_cast<double>(in.size()) - 1.0)));
    };
    auto fine = prepared(id, fs, 1, defaults(id));
    const auto reference = render(*fine, in, 1, defaults(id), sweep);
    auto coarse = prepared(id, fs, 64, defaults(id));
    const auto out = render(*coarse, in, 64, defaults(id), sweep);
    const double r = m::residualDbc(out, reference);

    constexpr int kStep = 12032; // a multiple of 64
    auto at = [&](double v) {
        auto p = defaults(id);
        set(p, v);
        return p;
    };
    auto lo = prepared(id, fs, 64, at(0.0));
    const auto low = render(*lo, in, 64, at(0.0));
    auto hi = prepared(id, fs, 64, at(10.0));
    const auto high = render(*hi, in, 64, at(10.0));
    auto st = prepared(id, fs, 64, at(0.0));
    const auto stepped =
        render(*st, in, 64, at(0.0), [&](int pos, AmpParams& p) { set(p, pos >= kStep ? 10.0 : 0.0); });
    const auto c = clicks(stepped, low, high, kStep, fs);
    std::printf("zipper %-8s knob %d: block-64 vs per-sample sweep %6.1f dBc; step: HF tilt x%.3f (hard x%.3f), "
                "envelope x%.3f\n",
                name(id), knob, r, c.tilt, c.spliceTilt, c.envelope);
    CHECK(r <= -60.0);
    CHECK(c.clean());
}

// AGENTS.md section 4: parameter and input fuzz (NaN, Inf, huge, subnormal, random knobs and switches per block)
// under the RT checker; the output stays finite and free of subnormals at every rate and quality.
TEST_CASE("ClassicAmp: parameter and input fuzz, NaN/Inf/subnormal scan, RT checker", "[engine][amp][classic][rt]")
{
    const auto id = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA);
    const double fs = GENERATE(44100.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(name(id), fs, q == OversamplingQuality::hq);
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN(), kInf = std::numeric_limits<float>::infinity();
    const float special[] = {kNan, kInf, -kInf, 1e30f, -1e30f, 1e-40f, 0.0f, 10.0f, -1.0f, 11.0f};
    std::vector<float> in(static_cast<std::size_t>(0.25 * fs));
    std::uint32_t seed = 99;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    auto pick = [&] { return special[static_cast<int>(rnd() * 10.0f) % 10]; };
    for (auto& v : in)
        v = rnd() < 0.002f ? pick() : 2.0f * rnd() - 1.0f;
    auto amp = prepared(id, fs, 127, defaults(id), q);
    const auto out = render(*amp, in, 127, defaults(id), [&](int, AmpParams& p) {
        float* knobs[] = {&p.gain, &p.bass, &p.mid, &p.treble, &p.presence, &p.depth, &p.master, &p.levelDb};
        for (float* k : knobs)
            *k = rnd() < 0.1f ? pick() : 12.0f * rnd() - 1.0f;
        p.bright = rnd() < 0.5f;
        p.tight = rnd() < 0.5f;
    });
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    // One second of silence afterwards decays without subnormals.
    const auto tail = render(*amp, std::vector<float>(static_cast<std::size_t>(fs), 0.0f), 127, defaults(id));
    OSP_REQUIRE_OUTPUT_CLEAN(tail);
}

// Research Q10 and ADR-0011's open item: the HQ (8x) cost at 96 kHz, next to Live. Informative (hidden from CI; the
// gated numbers are osp-bench's amp_classic_* entries).
TEST_CASE("ClassicAmp: CPU per model, Live and HQ at 48 and 96 kHz, 64-sample blocks", "[.cpu]")
{
    using Case = std::pair<double, OversamplingQuality>;
    for (const auto id : {ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA})
        for (const auto& [fs, q] : {Case{48000.0, OversamplingQuality::live}, Case{96000.0, OversamplingQuality::live},
                                    Case{48000.0, OversamplingQuality::hq}, Case{96000.0, OversamplingQuality::hq}}) {
            const auto p = defaults(id);
            auto amp = prepared(id, fs, 64, p, q);
            const auto in = m::makeTone(m::coherentTone(440.0, fs), fs, -12.0, static_cast<std::size_t>(fs));
            std::vector<float> out(in.size());
            double best = 1e9;
            for (int rep = 0; rep < 5; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                for (std::size_t pos = 0; pos < in.size(); pos += 64) {
                    amp->setParameters(p);
                    amp->process(in.data() + pos, out.data() + pos, 64);
                }
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            std::printf("cpu %-8s %5.0f Hz %s x%d: %.2f %% of one core\n", name(id), fs,
                        q == OversamplingQuality::hq ? "HQ  " : "Live", amp->getFactor(), 100.0 * best);
        }
}
