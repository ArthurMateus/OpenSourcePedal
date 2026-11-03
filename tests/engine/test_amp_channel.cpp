// ModelAmpChannel (#175 part 1, ADR-0011 P5-9, classic-first): one integer D_slot for every model and the pads that
// align them, model-change crossfades (click metric), the amp.ch<N>.lowCutHz response and smoothing (REQ-TEST-003),
// the rate x block matrix and parameter/model/input fuzz under the RT checker. The graph wiring and REQ-301/302 end
// to end follow in part 2.

#include "../../engine/src/graph/ModelAmpChannel.h"
#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <utility>
#include <vector>

using osp::dsp::OversamplingQuality;
using osp::engine::AmpParams;
using osp::engine::ClassicAmpId;
using osp::engine::ClassicAmpModel;
using osp::engine::ModelAmpChannel;
namespace m = osp::measure;

namespace {

constexpr ClassicAmpId kIds[] = {ClassicAmpId::cleanA, ClassicAmpId::crunchA, ClassicAmpId::leadA};

const char* name(ClassicAmpId id)
{
    return id == ClassicAmpId::cleanA ? "clean_a" : id == ClassicAmpId::crunchA ? "crunch_a" : "lead_a";
}
const char* name(OversamplingQuality q)
{
    return q == OversamplingQuality::hq ? "HQ  " : "Live";
}

AmpParams defaults(ClassicAmpId id)
{
    AmpParams p;
    p.gain = id == ClassicAmpId::cleanA ? 3.0f : id == ClassicAmpId::crunchA ? 5.0f : 6.0f;
    return p;
}

std::size_t settle(double fs) // 1 s: the amps' slowest poles (ClassicAmp test)
{
    return std::max<std::size_t>(m::kWarmupSamples, static_cast<std::size_t>(fs));
}

std::vector<float> add(std::vector<float> a, const std::vector<float>& b)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] += b[i];
    return a;
}

std::vector<double> tail(const std::vector<float>& x)
{
    return {x.end() - static_cast<long>(m::kCoherentFftSize), x.end()};
}

// Renders a channel in blocks under the RT checker; per(pos, params, channel) runs before each block.
using PerBlock = std::function<void(int, AmpParams&, ModelAmpChannel&)>;
std::vector<float> render(ModelAmpChannel& ch, std::vector<float> x, int block, AmpParams p, const PerBlock& per = {})
{
    const int total = static_cast<int>(x.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block) {
            if (per)
                per(pos, p, ch);
            ch.setParameters(p);
            ch.process(x.data() + pos, std::min(block, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(x);
    return x;
}

std::unique_ptr<ModelAmpChannel>
prepared(ClassicAmpId id, double fs, int block, const AmpParams& p, OversamplingQuality q = OversamplingQuality::live)
{
    auto ch = std::make_unique<ModelAmpChannel>(id);
    ch->setParameters(p);
    ch->prepare(fs, block, q);
    return ch;
}

// The ClassicAmp test's click metric: over [at - 50 ms, at + 100 ms] in 5 ms windows, the largest ratio of
// 2nd-difference energy to signal energy (`tilt`, a click is broadband) and the largest window RMS (`envelope`),
// relative to the larger of the two static renders; `spliceTilt` is the same for a hard cut at `at`.
// A swap between two differently voiced amps is a crossfade of two signals whose fundamentals are not in phase:
// the partial cancellation of the low-frequency energy mid-fade raises the tilt by itself (measured up to x1.31 for
// clean_a <-> lead_a, unchanged by a 5...50 ms warm-up of the incoming model, so it is not its cold start), while
// every hard cut measured x2.57 or more. Swaps are gated at x1.5 or half the hard cut; parameter changes at the
// ClassicAmp test's x1.25.
struct ClickReport {
    double tilt = 0.0, envelope = 0.0, spliceTilt = 0.0;
    bool clean(double bound = 1.25) const noexcept { return tilt <= bound || tilt <= 0.5 * spliceTilt; }
};
constexpr double kSwapTilt = 1.5;
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

} // namespace

// ADR-0011 D2/A2: D_slot = ceil(max processing delay of the three models) at every (fs, quality), the same in every
// channel; each model's pad is D_slot - D_k, measured on the running channel against the bare model (the pad is the
// only difference) from the 1 kHz phase, within 0.01 sample. A model change therefore crossfades aligned signals.
TEST_CASE("AmpChannel: one integer D_slot for every model, padded and measured at every rate and quality",
          "[engine][amp][channel][latency]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, q == OversamplingQuality::hq);
    double slot = -1.0;
    for (const auto home : kIds) {
        ModelAmpChannel ch(home);
        ch.prepare(fs, 512, q);
        if (slot < 0.0)
            slot = ch.getMeasuredDelaySamples();
        CHECK(ch.getMeasuredDelaySamples() == slot);
    }
    double maxD = 0.0;
    for (const auto id : kIds) {
        ClassicAmpModel bare(id);
        bare.setParameters(defaults(id));
        bare.prepare(fs, 512, q);
        maxD = std::max(maxD, bare.getMeasuredDelaySamples());
    }
    CHECK(slot == std::ceil(maxD - 1e-9));

    const auto tone = m::coherentTone(1000.0, fs);
    const auto in = m::makeTone(tone, fs, -24.0, settle(fs) / 4 + m::kCoherentFftSize);
    for (const auto id : kIds) {
        auto ch = prepared(ClassicAmpId::cleanA, fs, 512, defaults(id), q);
        ch->setModel(id);
        ch->reset(); // installs it directly
        REQUIRE(ch->activeModel() == id);
        const auto out = render(*ch, in, 512, defaults(id));
        ClassicAmpModel bare(id);
        bare.setParameters(defaults(id));
        bare.prepare(fs, 512, q);
        std::vector<float> ref(in.size());
        for (std::size_t pos = 0; pos < in.size(); pos += 512)
            bare.process(in.data() + pos, ref.data() + pos,
                         static_cast<int>(std::min<std::size_t>(512, in.size() - pos)));
        const auto w = 2.0 * std::numbers::pi * tone.frequency / fs;
        const double pad = -std::arg(m::realFft(tail(out))[tone.bin] / m::realFft(tail(ref))[tone.bin]) / w;
        std::printf("slot %5.0f Hz %s: %-8s D %7.4f, D_slot %2.0f, pad %6.4f (measured %6.4f)\n", fs, name(q), name(id),
                    bare.getMeasuredDelaySamples(), slot, ch->padSamples(id), pad);
        CHECK(ch->padSamples(id) >= 0.0);
        CHECK(std::abs(ch->padSamples(id) - (slot - bare.getMeasuredDelaySamples())) < 1e-12);
        CHECK(std::abs(pad - ch->padSamples(id)) < 0.01);
    }
}

// A model change crossfades over the ADR-0004 25 ms from a cold-started incoming model: from the end of the fade the
// output is bit-identical to a channel on which that model was installed by reset() at the same block, and the fade
// leaves no click (ClickReport::clean against the two static renders).
TEST_CASE("AmpChannel: a model change crossfades aligned over 25 ms, without a click", "[engine][amp][channel][swap]")
{
    const auto from = GENERATE(ClassicAmpId::cleanA, ClassicAmpId::leadA);
    const auto to = GENERATE(ClassicAmpId::crunchA, ClassicAmpId::leadA, ClassicAmpId::cleanA);
    if (from == to)
        return;
    CAPTURE(name(from), name(to));
    constexpr double fs = 48000.0;
    constexpr int kBlock = 64, kAt = 24000 + 37; // not block-aligned: the change lands on the next block start
    const int start = (kAt + kBlock - 1) / kBlock * kBlock;
    const auto in = m::makeTone(m::coherentTone(220.0, fs), fs, -12.0, 48000);
    const AmpParams p = defaults(ClassicAmpId::crunchA);

    auto staticRender = [&](ClassicAmpId id) {
        auto ch = prepared(from, fs, kBlock, p);
        ch->setModel(id);
        ch->reset();
        return render(*ch, in, kBlock, p);
    };
    const auto before = staticRender(from), after = staticRender(to);
    auto ch = prepared(from, fs, kBlock, p);
    const auto swapped = render(*ch, in, kBlock, p, [&](int pos, AmpParams&, ModelAmpChannel& c) {
        if (pos >= kAt)
            c.setModel(to);
    });
    CHECK(ch->activeModel() == to);
    CHECK_FALSE(ch->isSwapping());
    auto installed = prepared(from, fs, kBlock, p);
    const auto reference = render(*installed, in, kBlock, p, [&](int pos, AmpParams&, ModelAmpChannel& c) {
        if (pos == start) {
            c.setModel(to);
            c.reset();
        }
    });
    const int fadeEnd = start + static_cast<int>(std::lround(ModelAmpChannel::kSwapSeconds * fs));
    CHECK(std::equal(swapped.begin() + fadeEnd, swapped.end(), reference.begin() + fadeEnd));
    CHECK_FALSE(std::equal(swapped.begin() + start, swapped.begin() + fadeEnd - 1, reference.begin() + start));
    const auto c = clicks(swapped, before, after, start, fs);
    std::printf("model change %-8s -> %-8s: HF tilt x%.3f (hard cut x%.3f), envelope x%.3f of static\n", name(from),
                name(to), c.tilt, c.spliceTilt, c.envelope);
    CHECK(c.clean(kSwapTilt));
}

// ADR-0011 D4: the low cut is a 2nd-order Butterworth high-pass at the channel input. Through clean_a at -120 dBFS
// (linear), the ratio of the outputs with lowCutHz = fc and = 20 (off) is the bilinear Butterworth response
// |H|^2 = r^4 / (1 + r^4), r = tan(pi f / fs) / tan(pi fc / fs), within 0.02 dB. levelDb scales the output only.
TEST_CASE("AmpChannel: lowCutHz is a 12 dB/oct Butterworth high-pass at the input; levelDb", "[engine][amp][channel]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const float fc = GENERATE(80.0f, 300.0f);
    CAPTURE(fs, fc);
    std::vector<m::CoherentTone> tones;
    for (const double f : {0.5 * fc, 1.0 * fc, 2.0 * fc, 4.0 * fc, 1000.0})
        tones.push_back(m::coherentTone(f, fs));
    const std::size_t n = settle(fs) + m::kCoherentFftSize;
    std::vector<float> in(n, 0.0f);
    for (const auto& t : tones)
        in = add(std::move(in), m::makeTone(t, fs, -120.0, n));
    auto run = [&](float lowCut, float levelDb) {
        auto p = defaults(ClassicAmpId::cleanA);
        p.lowCutHz = lowCut;
        p.levelDb = levelDb;
        auto ch = prepared(ClassicAmpId::cleanA, fs, 512, p);
        return m::realFft(tail(render(*ch, in, 512, p)));
    };
    const auto off = run(20.0f, 0.0f), on = run(fc, 0.0f), quiet = run(20.0f, -6.0f);
    for (const auto& t : tones) {
        const double r = std::tan(std::numbers::pi * t.frequency / fs) / std::tan(std::numbers::pi * fc / fs);
        const double expected = 10.0 * std::log10(std::pow(r, 4) / (1.0 + std::pow(r, 4)));
        const double got = m::toDb(std::abs(on[t.bin]) / std::abs(off[t.bin]));
        std::printf("lowCut %5.0f Hz fc %3.0f: %7.1f Hz %8.3f dB (expected %8.3f)\n", fs, fc, t.frequency, got,
                    expected);
        CHECK(std::abs(got - expected) < 0.02);
        CHECK(std::abs(m::toDb(std::abs(quiet[t.bin]) / std::abs(off[t.bin])) + 6.0) < 0.01);
    }
}

// REQ-TEST-003 for lowCutHz: (a) a full-range step 20 -> 300 Hz on a non-block-aligned sample, against an offline
// double reference of the specified law (20 ms one-pole on the frequency, 20 ms linear off -> on mix, per-sample
// Butterworth TPT SVF) fed through an otherwise identical channel with the low cut off: <= -60 dBc at -100 dBFS
// (linear amp); (b) the same step at -12 dBFS leaves no click; (c) a 20 -> 300 -> 20 Hz sweep with targets every
// 64 samples renders bit-identically in 64- and 1-sample blocks.
TEST_CASE("AmpChannel: lowCutHz smoothing, step residual, no click, block-size invariant",
          "[engine][amp][channel][zipper]")
{
    constexpr double fs = 48000.0;
    constexpr int kStep = 24000 + 37, kTotal = 48000;
    const auto id = ClassicAmpId::cleanA;
    auto stepAt = [&](int pos, AmpParams& p, ModelAmpChannel&) { p.lowCutHz = pos >= kStep ? 300.0f : 20.0f; };

    SECTION("residual against the double reference")
    {
        const auto x = m::makeTone(m::coherentTone(110.0, fs), fs, -100.0, kTotal);
        auto ch = prepared(id, fs, 1, defaults(id));
        const auto out = render(*ch, x, 1, defaults(id), stepAt);
        // The reference law, double precision, from the delivery sample.
        std::vector<float> filtered(x.size());
        const double a = std::exp(-1.0 / (ModelAmpChannel::kLowCutSeconds * fs));
        const double len = std::lround(ModelAmpChannel::kLowCutSeconds * fs);
        double ic1 = 0.0, ic2 = 0.0, hz = 20.0;
        for (int i = 0; i < kTotal; ++i) {
            const double v0 = x[static_cast<std::size_t>(i)];
            if (i < kStep) {
                filtered[static_cast<std::size_t>(i)] = static_cast<float>(v0);
                continue;
            }
            hz = 300.0 + (hz - 300.0) * a;
            const double g = std::tan(std::numbers::pi * hz / fs), k = std::sqrt(2.0);
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            const double hp = v0 - k * v1 - v2, mix = std::min(1.0, (i - kStep + 1) / len);
            filtered[static_cast<std::size_t>(i)] = static_cast<float>(v0 + mix * (hp - v0));
        }
        auto refCh = prepared(id, fs, 1, defaults(id));
        const auto ref = render(*refCh, filtered, 1, defaults(id));
        const double dbc = m::residualDbc(out, ref);
        std::printf("lowCut step 20 -> 300 Hz: residual %.1f dBc vs the double reference\n", dbc);
        CHECK(dbc <= -60.0);
    }
    SECTION("no click at -12 dBFS")
    {
        const auto x = m::makeTone(m::coherentTone(110.0, fs), fs, -12.0, kTotal);
        auto with = [&](float hz) {
            auto p = defaults(id);
            p.lowCutHz = hz;
            auto ch = prepared(id, fs, 1, p);
            return render(*ch, x, 64, p);
        };
        const auto before = with(20.0f), after = with(300.0f);
        auto ch = prepared(id, fs, 1, defaults(id));
        const auto stepped = render(*ch, x, 1, defaults(id), stepAt);
        const auto c = clicks(stepped, before, after, kStep, fs);
        std::printf("lowCut step 20 -> 300 Hz: HF tilt x%.3f (hard cut x%.3f), envelope x%.3f\n", c.tilt, c.spliceTilt,
                    c.envelope);
        CHECK(c.clean());
    }
    SECTION("sweep, 64- vs 1-sample blocks")
    {
        std::mt19937 rng(3u);
        std::uniform_real_distribution<float> dist(-0.3f, 0.3f);
        std::vector<float> x(static_cast<std::size_t>(kTotal));
        for (auto& v : x)
            v = dist(rng);
        auto sweep = [&](int pos, AmpParams& p, ModelAmpChannel&) {
            if (pos % 64 == 0) {
                const double t = static_cast<double>(pos) / (0.5 * fs); // 0..2
                p.lowCutHz = static_cast<float>(20.0 * std::pow(15.0, t <= 1.0 ? t : std::max(0.0, 2.0 - t)));
            }
        };
        auto a = prepared(id, fs, 64, defaults(id));
        auto b = prepared(id, fs, 64, defaults(id));
        CHECK(render(*a, x, 64, defaults(id), sweep) == render(*b, x, 1, defaults(id), sweep));
    }
}

// REQ-TEST-001: every rate x block size renders bit-identically (low cut on, bright on, lead_a); reset() repeats it.
TEST_CASE("AmpChannel: rate x block-size matrix and reset() determinism", "[engine][amp][channel]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    auto p = defaults(ClassicAmpId::leadA);
    p.lowCutHz = 150.0f;
    p.bright = true;
    std::mt19937 rng(5u);
    std::uniform_real_distribution<float> dist(-0.2f, 0.2f);
    std::vector<float> x(8192);
    for (auto& v : x)
        v = dist(rng);
    auto ch = prepared(ClassicAmpId::leadA, fs, 2048, p);
    const auto reference = render(*ch, x, 64, p);
    for (const int block : {1, 32, 127, 512, 2048}) {
        CAPTURE(block);
        ch->reset();
        CHECK(render(*ch, x, block, p) == reference);
    }
}

// Channel fuzz: random knobs, NaN/Inf/huge values, model changes and block sizes 0...maxBlock, and NaN/Inf/1e30
// input samples, at every rate and quality; the output stays finite and normal, under the RT checker.
TEST_CASE("AmpChannel: parameter, model and input fuzz, NaN/Inf/subnormal scan, RT checker",
          "[engine][amp][channel][rt]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    CAPTURE(fs, q == OversamplingQuality::hq);
    constexpr int kMax = 512;
    ModelAmpChannel ch(ClassicAmpId::crunchA);
    ch.prepare(fs, kMax, q);
    std::mt19937 rng(static_cast<unsigned>(fs) + (q == OversamplingQuality::hq ? 1u : 0u));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    constexpr float kSpecial[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f};
    auto knob = [&](float lo, float hi) {
        const float u = unit(rng);
        return u < 0.1f ? kSpecial[static_cast<std::size_t>(u * 50.0f) % 5] : lo + unit(rng) * (hi - lo);
    };
    std::vector<float> x(static_cast<std::size_t>(fs)), buf(kMax);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    for (std::size_t i = 997; i < x.size(); i += 4099)
        x[i] = kSpecial[i % 5];
    std::size_t nonFinite = 0, subnormal = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (std::size_t pos = 0; pos < x.size();) {
            AmpParams p;
            for (float* k : {&p.gain, &p.bass, &p.mid, &p.treble, &p.presence, &p.depth, &p.master})
                *k = knob(0, 10);
            p.levelDb = knob(-24, 12);
            p.lowCutHz = knob(20, 300);
            p.bright = unit(rng) < 0.5f;
            p.tight = unit(rng) < 0.5f;
            ch.setParameters(p);
            if (unit(rng) < 0.05f)
                ch.setModel(kIds[static_cast<std::size_t>(unit(rng) * 2.999f)]);
            const int n = std::min(static_cast<int>(unit(rng) * (kMax + 1)), static_cast<int>(x.size() - pos));
            std::copy_n(x.begin() + static_cast<long>(pos), n, buf.begin());
            ch.process(buf.data(), n);
            for (int i = 0; i < n; ++i) {
                nonFinite += std::isfinite(buf[static_cast<std::size_t>(i)]) ? 0u : 1u;
                subnormal += std::fpclassify(buf[static_cast<std::size_t>(i)]) == FP_SUBNORMAL ? 1u : 0u;
            }
            pos += static_cast<std::size_t>(std::max(n, 1));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0u);
    CHECK(subnormal == 0u);
}
