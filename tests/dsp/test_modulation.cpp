// The `mod` block (#70 part 4): type dispatch and 30 ms type crossfade, pre (mono) / post (stereo), tempo sync.

#include "osp/dsp/Modulation.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <type_traits>
#include <vector>

using osp::dsp::Modulation;
using Type = osp::dsp::Modulation::Type;

namespace {

constexpr int kMaxBlock = 2048;
constexpr std::array kTypes{Type::chorus80s, Type::chorusWide, Type::flanger, Type::phaser, Type::vibe, Type::tremolo};

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

// Renders [from, to) in blocks through the RT checker. `r` empty = processMono (the pre position).
template <typename Fx>
void render(Fx& fx, std::vector<float>& l, std::vector<float>& r, int block, int from = 0, int to = -1)
{
    const int total = to < 0 ? static_cast<int>(l.size()) : to;
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = from; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            if constexpr (std::is_same_v<Fx, Modulation>) {
                if (r.empty()) {
                    if (block == 0)
                        fx.processMono(l.data() + pos, 0);
                    fx.processMono(l.data() + pos, n);
                } else {
                    if (block == 0)
                        fx.processStereo(l.data() + pos, r.data() + pos, 0);
                    fx.processStereo(l.data() + pos, r.data() + pos, n);
                }
            } else {
                fx.process(l.data() + pos, r.data() + pos, n);
            }
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

// The standalone engine a type maps to, configured like the block, rendered over [from, end).
void renderEngine(Type t, double fs, std::vector<float>& l, std::vector<float>& r, float rateHz, int from = 0)
{
    auto run = [&](auto& e) {
        e.setRateHz(rateHz);
        e.setDepthPct(70.0f);
        e.setMixPct(60.0f);
        e.setWidthPct(80.0f);
        e.prepare(fs, kMaxBlock);
        render(e, l, r, 64, from);
    };
    if (t == Type::phaser || t == Type::vibe) {
        osp::dsp::Phaser p;
        p.setVoicing(t == Type::phaser ? osp::dsp::Phaser::Voicing::phaser : osp::dsp::Phaser::Voicing::vibe);
        p.setFeedbackPct(30.0f);
        run(p);
    } else if (t == Type::tremolo) {
        osp::dsp::Tremolo tr;
        run(tr);
    } else {
        osp::dsp::ChorusFlanger c;
        c.setVoicing(t == Type::chorus80s    ? osp::dsp::ChorusFlanger::Voicing::chorus80s
                     : t == Type::chorusWide ? osp::dsp::ChorusFlanger::Voicing::chorusWide
                                             : osp::dsp::ChorusFlanger::Voicing::flanger);
        c.setFeedbackPct(30.0f);
        run(c);
    }
}

Modulation makeBlock(Type t, double fs, int maxBlock = kMaxBlock)
{
    Modulation m;
    m.setType(t);
    m.setDepthPct(70.0f);
    m.setMixPct(60.0f);
    m.setWidthPct(80.0f);
    m.setFeedbackPct(30.0f);
    m.prepare(fs, maxBlock);
    return m;
}

} // namespace

TEST_CASE("Modulation: every type is its engine, bit-exact, at every rate and block size", "[dsp][mod]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 127, 2048);
    CAPTURE(fs, block);
    const int n = static_cast<int>(0.1 * fs);
    const auto a = sine(220.0, fs, n, 0.4), b = sine(1330.0, fs, n, 0.3);
    for (Type t : kTypes) {
        auto m = makeBlock(t, fs);
        REQUIRE(m.getLatencySamples() == 0);
        REQUIRE(m.type() == t);
        auto l = a, r = b, el = a, er = b;
        render(m, l, r, block);
        renderEngine(t, fs, el, er, 0.8f);
        CAPTURE(static_cast<int>(t));
        CHECK(l == el);
        CHECK(r == er);
    }
}

TEST_CASE("Modulation: a type change crossfades the old engine into a fresh new one over 30 ms", "[dsp][mod]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(0.2 * fs), step = 4801; // not block-aligned
    const auto a = sine(220.0, fs, n, 0.4), b = sine(1330.0, fs, n, 0.3);
    auto m = makeBlock(Type::chorusWide, fs);
    auto l = a, r = b;
    render(m, l, r, 64, 0, step);
    m.setType(Type::tremolo);
    REQUIRE(m.isFading());
    render(m, l, r, 64, step);
    CHECK_FALSE(m.isFading());
    // Reference: (1 − g)·old + g·new, g the 30 ms linear ramp; the new engine starts reset at the change.
    auto oldL = a, oldR = b, newL = a, newR = b;
    renderEngine(Type::chorusWide, fs, oldL, oldR, 0.8f);
    renderEngine(Type::tremolo, fs, newL, newR, 0.8f, step);
    osp::dsp::LinearRamp g{0.0f};
    g.prepare(fs, Modulation::kTypeFadeSeconds);
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i == static_cast<std::size_t>(step))
            g.setTarget(1.0f);
        const float k = i < static_cast<std::size_t>(step) ? 0.0f : g.next();
        worst = std::max({worst, std::abs(l[i] - ((1.0f - k) * oldL[i] + k * newL[i])),
                          std::abs(r[i] - ((1.0f - k) * oldR[i] + k * newR[i]))});
    }
    CHECK(worst <= 1e-6f);
    CHECK(l.back() == newL.back()); // settled: exactly the new engine

    // A change during a fade is applied when that fade ends.
    m.setType(Type::phaser);
    std::vector<float> x(static_cast<std::size_t>(0.01 * fs), 0.1f), y = x;
    render(m, x, y, 64);
    m.setType(Type::vibe);
    CHECK(m.type() == Type::vibe);
    std::vector<float> x2(static_cast<std::size_t>(0.1 * fs), 0.1f), y2 = x2;
    render(m, x2, y2, 64);
    CHECK_FALSE(m.isFading());
    CHECK(m.type() == Type::vibe);
}

TEST_CASE("Modulation: pre position is the mono sum, dry paths exact, chunking, sync", "[dsp][mod]")
{
    const double fs = 44100.0;
    const int n = static_cast<int>(0.2 * fs);
    const auto a = sine(330.0, fs, n, 0.5);
    std::vector<float> none;

    SECTION("processMono = the average of the stereo engine's two sides on a duplicated input")
    {
        for (Type t : kTypes) {
            auto mono = makeBlock(t, fs), stereo = makeBlock(t, fs);
            mono.setPosition(Modulation::Position::pre);
            CHECK(mono.position() == Modulation::Position::pre);
            auto x = a, l = a, r = a;
            render(mono, x, none, 127);
            render(stereo, l, r, 127);
            bool same = true;
            for (std::size_t i = 0; i < a.size(); ++i)
                same = same && x[i] == 0.5f * (l[i] + r[i]);
            CAPTURE(static_cast<int>(t));
            CHECK(same);
        }
    }
    SECTION("disabled and bypassed are exact dry, mono and stereo")
    {
        for (int mode = 0; mode < 2; ++mode) {
            auto m = makeBlock(Type::flanger, fs);
            if (mode == 0) {
                m.setEnabled(false);
                m.reset();
            } else {
                m.setBypassed(true);
            }
            auto x = a, l = a, r = a;
            render(m, x, none, 64);
            render(m, l, r, 64);
            CHECK(x == a);
            CHECK(l == a);
            CHECK(r == a);
        }
    }
    SECTION("calls longer than maxBlockSize are chunked with no change in output")
    {
        auto small = makeBlock(Type::chorus80s, fs, 64), big = makeBlock(Type::chorus80s, fs);
        auto l1 = a, r1 = a, l2 = a, r2 = a;
        render(small, l1, r1, 1000);
        render(big, l2, r2, 1000);
        CHECK(l1 == l2);
        CHECK(r1 == r2);
    }
    SECTION("sync: beats from the parameters.md values, rate = bpm / 60 / beats")
    {
        CHECK(Modulation::syncBeats("off") == 0.0f);
        CHECK(Modulation::syncBeats("1/1") == 4.0f);
        CHECK(Modulation::syncBeats("1/4") == 1.0f);
        CHECK(Modulation::syncBeats("1/8D") == 0.75f);
        CHECK(std::abs(Modulation::syncBeats("1/16T") - 1.0f / 6.0f) < 1e-6f);
        CHECK(Modulation::syncBeats("1/3") == 0.0f);
        auto m = makeBlock(Type::tremolo, fs);
        m.setRateHz(5.0f);
        m.setTempoBpm(120.0f);
        m.setSyncBeats(Modulation::syncBeats("1/4"));
        CHECK(m.effectiveRateHz() == 2.0f);
        m.reset();
        auto l = a, r = a, el = a, er = a;
        render(m, l, r, 64);
        renderEngine(Type::tremolo, fs, el, er, 2.0f);
        CHECK(l == el);
        m.setSyncBeats(Modulation::syncBeats("1/16T")); // 12 Hz: clamped to the 10 Hz maximum
        CHECK(m.effectiveRateHz() == 10.0f);
        m.setSyncBeats(0.0f);
        CHECK(m.effectiveRateHz() == 5.0f);
    }
}

TEST_CASE("Modulation: reset(), before prepare(), 1000 random vectors with type, position and sync", "[dsp][mod]")
{
    Modulation cold;
    std::vector<float> cl(64, 0.5f), cr(64, 0.5f);
    cold.processStereo(cl.data(), cr.data(), 64);
    cold.processMono(cr.data(), 64);
    CHECK(cl == std::vector<float>(64, 0.0f));
    CHECK(cr == std::vector<float>(64, 0.0f));

    auto m = makeBlock(Type::vibe, 48000.0, 512);
    const auto s = sine(150.0, 48000.0, 9600, 0.5);
    auto l1 = s, r1 = s, l2 = s, r2 = s;
    render(m, l1, r1, 64, 0, 2000);
    m.setType(Type::flanger); // reset() mid-fade settles on the new type
    m.reset();
    render(m, l1, r1, 64, 2000);
    m.reset(); // a fresh flanger again, rendered over the same samples
    render(m, l2, r2, 64, 2000);
    CHECK(std::equal(l1.begin() + 2000, l1.end(), l2.begin() + 2000));
    CHECK(std::equal(r1.begin() + 2000, r1.end(), r2.begin() + 2000));

    std::mt19937 rng(7004);
    std::uniform_real_distribution<float> pct(-120.0f, 120.0f), hz(-1.0f, 15.0f), bpm(-10.0f, 300.0f), sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, 3000), coin(0, 1), type(0, 5), beats(0, 12);
    std::vector<float> bl(3000), br(3000);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            m.setType(kTypes[static_cast<std::size_t>(type(rng))]);
            m.setRateHz(k % 71 == 0 ? std::nanf("") : hz(rng));
            m.setTempoBpm(k % 53 == 0 ? std::nanf("") : bpm(rng));
            m.setSyncBeats(k % 43 == 0 ? std::nanf("") : 0.25f * static_cast<float>(beats(rng)));
            m.setDepthPct(pct(rng));
            m.setMixPct(k % 59 == 0 ? std::nanf("") : pct(rng));
            m.setFeedbackPct(pct(rng));
            m.setWidthPct(pct(rng));
            m.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            const bool mono = coin(rng) == 1;
            if (mono)
                m.processMono(bl.data(), n);
            else
                m.processStereo(bl.data(), br.data(), n);
            for (int i = 0; i < n; ++i)
                nonFinite += std::isfinite(bl[static_cast<std::size_t>(i)]) &&
                                     (mono || std::isfinite(br[static_cast<std::size_t>(i)]))
                                 ? 0
                                 : 1;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0);
}
