// Setter idempotence and host-block-size independence (#161, root cause of #159). The engine re-sends every
// parameter at the start of every block, so a setter called again with the value it already has must be a no-op:
// the output may not depend on how often it is called, nor on the host block size. Gate: bit-identical (0
// difference) against one reference render. A tolerance is not needed: with setters idempotent, every block below
// is a per-sample recursion whose state does not depend on where the block boundaries fall.

#include "osp/dsp/Compressor.h"
#include "osp/dsp/Delay.h"
#include "osp/dsp/Doubler.h"
#include "osp/dsp/Drive.h"
#include "osp/dsp/Eq.h"
#include "osp/dsp/Modulation.h"
#include "osp/dsp/Reverb.h"
#include "osp/dsp/util/Smoothers.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace {

using namespace osp::dsp;

constexpr int kMaxBlock = 2048;
constexpr int kRefBlock = 64;

// 0.2 · sine 220 Hz plus a decaying noise burst every 250 ms (a plucked-string stand-in, deterministic).
std::vector<float> testInput(double fs, int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const int period = static_cast<int>(0.25 * fs);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double env = std::exp(-static_cast<double>(i % period) / (0.03 * fs));
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 220.0 * i / fs) +
                                                            0.5 * env * static_cast<double>(u(rng)));
    }
    return x;
}

double maxAbsDiffDb(const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return m > 0.0 ? 20.0 * std::log10(m) : -400.0;
}

// A stereo render: `F` has prepare(fs, maxBlock), set() (the setters under test) and process(l, r, n). With
// `resend` the setters are called again at the start of every block, as the engine does; otherwise once, before the
// first block (so the smoothers glide from their defaults in both cases).
template <typename F>
void render(double fs, int seconds10, int block, bool resend, std::vector<float>& l, std::vector<float>& r)
{
    const int n = static_cast<int>(fs * seconds10 / 10);
    l = testInput(fs, n, 1u);
    r = testInput(fs, n, 2u);
    F f;
    f.prepare(fs, kMaxBlock);
    f.set();
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < n; pos += block) {
            if (resend)
                f.set();
            f.process(l.data() + pos, r.data() + pos, std::min(block, n - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

template <typename F>
void requireBlockSizeIndependent(double fs, int seconds10)
{
    std::vector<float> refL, refR, l, r;
    render<F>(fs, seconds10, kRefBlock, false, refL, refR);
    // The block is really processing (the left channel differs from the dry input by more than -40 dBFS).
    const double wetDb = maxAbsDiffDb(refL, testInput(fs, static_cast<int>(refL.size()), 1u));
    CAPTURE(wetDb);
    REQUIRE(wetDb > -40.0);
    for (const int block : {1, 16, 64, 127, 2048}) {
        render<F>(fs, seconds10, block, true, l, r);
        const double dl = maxAbsDiffDb(l, refL), dr = maxAbsDiffDb(r, refR);
        CAPTURE(fs, block, dl, dr);
        REQUIRE(l == refL);
        REQUIRE(r == refR);
    }
}

template <Delay::Mode M, bool Sync>
struct DelayFx {
    Delay d;
    void prepare(double fs, int mb) { d.prepare(fs, mb); }
    void set() noexcept
    {
        d.setMode(M);
        d.setTimeMs(1503.5f); // default 400 ms: a long glide of a large sample count
        d.setSync(Sync ? SyncDivision::n1_8d : SyncDivision::off);
        d.setTempoBpm(165.0);
        d.setFeedbackPct(35.0f);
        d.setMixPct(50.0f);
        d.setLowCutHz(120.0f);
        d.setHighCutHz(4000.0f);
        d.setDuckDb(6.0f);
    }
    void process(float* l, float* r, int n) noexcept { d.process(l, r, n); }
};

template <Reverb::Mode M>
struct ReverbFx {
    Reverb rv;
    void prepare(double fs, int mb) { rv.prepare(fs, mb); }
    void set() noexcept
    {
        Reverb::Params p;
        p.mode = M;
        p.preDelayMs = 100.0f; // default 20 ms
        p.mixPct = 50.0f;
        p.lowCutHz = 260.0f;
        p.highCutHz = 5000.0f;
        p.decayS = 1.5f;
        p.shimmerPct = M == Reverb::Mode::Shimmer ? 50.0f : 0.0f;
        rv.setParameters(p);
        rv.setEnabled(true);
    }
    void process(float* l, float* r, int n) noexcept { rv.process(l, r, l, r, n); }
};

struct DoublerFx {
    Doubler d;
    void prepare(double fs, int mb) { d.prepare(fs, mb); }
    void set() noexcept
    {
        d.setDelayMs(20.0f);
        d.setDetuneCents(12.5f);
    }
    void process(float* l, float* r, int n) noexcept { d.process(l, r, n); }
};

struct ModFx {
    Modulation m;
    void prepare(double fs, int mb) { m.prepare(fs, mb); }
    void set() noexcept
    {
        m.setRateHz(5.05f);
        m.setDepthPct(70.0f);
    }
    void process(float* l, float* r, int n) noexcept { m.processStereo(l, r, n); }
};

struct CompFx { // mono: left only, right passes through unchanged
    Compressor c;
    void prepare(double fs, int mb) { c.prepare(fs, mb); }
    void set() noexcept
    {
        c.setThresholdDb(-25.0f);
        c.setTone(7.0f);
    }
    void process(float* l, float*, int n) noexcept { c.process(l, l, n); }
};

struct DriveFx { // one stage at the base rate, left only
    DriveStage s;
    void prepare(double fs, int mb) { s.prepare(fs, mb); }
    void set() noexcept
    {
        s.setDrive(6.5f);
        s.setTone(3.0f);
        s.setLowCutHz(160.0f);
    }
    void process(float* l, float*, int n) noexcept { s.process(l, n); }
};

struct EqFx {
    Eq e;
    void prepare(double fs, int mb) { e.prepare(fs, mb); }
    void set() noexcept
    {
        e.setMode(EqMode::parametric);
        e.setParametric(1, 800.0f, 6.0f, 1.2f);
        e.setLowCutHz(260.0f);
        e.setHighCutHz(7000.0f);
    }
    void process(float* l, float* r, int n) noexcept
    {
        float* ch[2] = {l, r};
        e.process(ch, 2, n);
    }
};

} // namespace

TEST_CASE("OnePoleSmoother: re-sending the same target is a no-op (#161)", "[dsp][util][idempotence]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const float target = GENERATE(0.5f, 160.0f, 14400.0f, 72168.0f); // 72168 samples = 1.5 s at 48 kHz
    const int every = GENERATE(1, 7, 16, 32, 64, 127);
    CAPTURE(fs, target, every);

    const int n = static_cast<int>(2.0 * fs); // 2 s = 25 τ: the glide and the settled tail
    OnePoleSmoother once, resent;
    once.prepare(fs, 0.080);
    resent.prepare(fs, 0.080);
    once.setTarget(target);
    resent.setTarget(target);
    bool identical = true;
    float worst = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i % every == 0)
            resent.setTarget(target);
        const float a = once.next(), b = resent.next();
        identical = identical && a == b;
        worst = std::max(worst, std::abs(a - b));
    }
    CAPTURE(worst);
    REQUIRE(identical);
    REQUIRE(resent.current() == target);
    REQUIRE_FALSE(resent.isSmoothing());

    // A real change still glides from the current value, and re-sending it afterwards changes nothing.
    OnePoleSmoother a, b;
    a.prepare(fs, 0.020);
    b.prepare(fs, 0.020);
    for (auto* s : {&a, &b}) {
        s->setTarget(target);
        for (int i = 0; i < 100; ++i)
            s->next();
        s->setTarget(-target);
    }
    bool same = true;
    for (int i = 0; i < n; ++i) {
        b.setTarget(-target);
        same = same && a.next() == b.next();
    }
    REQUIRE(same);
    REQUIRE(a.current() == -target);
}

TEST_CASE("Setters re-sent every block: output independent of the host block size (#159, #161)", "[dsp][idempotence]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    SECTION("Delay digital, timeMs")
    {
        requireBlockSizeIndependent<DelayFx<Delay::Mode::digital, false>>(fs, 25);
    }
    SECTION("Delay analog, timeMs")
    {
        requireBlockSizeIndependent<DelayFx<Delay::Mode::analog, false>>(fs, 25);
    }
    SECTION("Delay tape, timeMs")
    {
        requireBlockSizeIndependent<DelayFx<Delay::Mode::tape, false>>(fs, 25);
    }
    SECTION("Delay digital, tempo sync")
    {
        requireBlockSizeIndependent<DelayFx<Delay::Mode::digital, true>>(fs, 15);
    }
    SECTION("Reverb room, preDelayMs")
    {
        requireBlockSizeIndependent<ReverbFx<Reverb::Mode::Room>>(fs, 10);
    }
    SECTION("Reverb hall, preDelayMs")
    {
        requireBlockSizeIndependent<ReverbFx<Reverb::Mode::Hall>>(fs, 10);
    }
    SECTION("Reverb plate, preDelayMs")
    {
        requireBlockSizeIndependent<ReverbFx<Reverb::Mode::Plate>>(fs, 10);
    }
    SECTION("Reverb spring, preDelayMs")
    {
        requireBlockSizeIndependent<ReverbFx<Reverb::Mode::Spring>>(fs, 10);
    }
    SECTION("Reverb shimmer, preDelayMs")
    {
        requireBlockSizeIndependent<ReverbFx<Reverb::Mode::Shimmer>>(fs, 10);
    }
    SECTION("Doubler, delayMs and detuneCents")
    {
        requireBlockSizeIndependent<DoublerFx>(fs, 10);
    }
    SECTION("Modulation, rateHz")
    {
        requireBlockSizeIndependent<ModFx>(fs, 10);
    }
    SECTION("Compressor, thresholdDb")
    {
        requireBlockSizeIndependent<CompFx>(fs, 10);
    }
    SECTION("Drive stage, lowCutHz")
    {
        requireBlockSizeIndependent<DriveFx>(fs, 10);
    }
    SECTION("Eq, lowCutHz")
    {
        requireBlockSizeIndependent<EqFx>(fs, 10);
    }
}
