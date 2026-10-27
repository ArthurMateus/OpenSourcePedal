// Tests for the Reverb block wrapper (issue #72, part 2): pre-delay, input cuts, mix, enable with trails, hard
// bypass, mode crossfade, and the RT fuzz. The tank itself is covered by test_fdn_tank.cpp.

#include "osp/dsp/Reverb.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {

using osp::dsp::Reverb;
constexpr int kMaxBlock = 2048;

struct Stereo {
    std::vector<float> l, r;
};

std::vector<float> noise(std::size_t n, std::uint32_t seed, float amplitude = 0.5f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-amplitude, amplitude);
    std::vector<float> x(n);
    for (auto& v : x)
        v = u(rng);
    return x;
}

Reverb makeReverb(double fs, const Reverb::Params& p = {})
{
    Reverb r;
    r.prepare(fs, kMaxBlock);
    r.setParameters(p);
    r.setEnabled(true);
    r.reset(); // settle every smoother and ramp on the new values
    return r;
}

// Renders in blocks of `blockSize` (0 = an empty call before each 64-sample block) under the RT checker. `before`
// runs at the start of every block with the block's first sample index.
template <typename Before>
Stereo render(Reverb& r, const std::vector<float>& inL, const std::vector<float>& inR, int blockSize, Before&& before)
{
    Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};
    const int total = static_cast<int>(inL.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            before(r, pos);
            const int n = std::min(step, total - pos);
            if (blockSize == 0)
                r.process(inL.data() + pos, inR.data() + pos, out.l.data() + pos, out.r.data() + pos, 0);
            r.process(inL.data() + pos, inR.data() + pos, out.l.data() + pos, out.r.data() + pos, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out.l);
    OSP_REQUIRE_OUTPUT_CLEAN(out.r);
    return out;
}

Stereo render(Reverb& r, const std::vector<float>& inL, const std::vector<float>& inR, int blockSize)
{
    return render(r, inL, inR, blockSize, [](Reverb&, int) {});
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i)
        e += double(x[i]) * x[i];
    return e;
}

} // namespace

TEST_CASE("Reverb: rate x block-size matrix, reset() bit-identical, pre-prepare", "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto n = static_cast<std::size_t>(0.25 * fs);
    auto inL = noise(n, 1u), inR = noise(n, 2u);
    std::fill(inL.begin() + static_cast<long>(n / 2), inL.end(), 0.0f);
    std::fill(inR.begin() + static_cast<long>(n / 2), inR.end(), 0.0f);
    Reverb ref = makeReverb(fs);
    const auto expected = render(ref, inL, inR, 64);
    for (int block : {0, 1, 32, 127, 512, 2048}) {
        CAPTURE(fs, block);
        Reverb r = makeReverb(fs);
        const auto y = render(r, inL, inR, block);
        REQUIRE(y.l == expected.l);
        REQUIRE(y.r == expected.r);
        r.reset();
        REQUIRE(render(r, inL, inR, block).l == expected.l);
    }

    Reverb unprepared;
    std::vector<float> x(16, 1.0f), l(16, 1.0f), rr(16, 1.0f);
    unprepared.process(x.data(), x.data(), l.data(), rr.data(), 16);
    REQUIRE(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(rr.begin(), rr.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Reverb: mix 0 is the exact dry signal, in place too; hard bypass is an exact copy", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const auto inL = noise(9600, 3u), inR = noise(9600, 4u);
    Reverb fresh = makeReverb(fs); // not hard-bypassed by default: the default 20 % mix adds a wet signal
    REQUIRE(render(fresh, inL, inR, 64).l != inL);
    Reverb::Params p;
    p.mixPct = 0.0f;
    Reverb r = makeReverb(fs, p);
    const auto y = render(r, inL, inR, 64);
    REQUIRE(y.l == inL);
    REQUIRE(y.r == inR);

    Reverb inPlace = makeReverb(fs, p);
    auto l = inL, rr = inR;
    for (std::size_t pos = 0; pos < l.size(); pos += 64)
        inPlace.process(l.data() + pos, rr.data() + pos, l.data() + pos, rr.data() + pos, 64);
    REQUIRE(l == inL);
    REQUIRE(rr == inR);

    Reverb wet = makeReverb(fs); // default 20 % mix
    wet.setHardBypass(true);
    const auto b = render(wet, inL, inR, 64);
    const auto settled = static_cast<std::size_t>(0.011 * fs); // the 10 ms hard-bypass fade
    REQUIRE(std::equal(b.l.begin() + static_cast<long>(settled), b.l.end(), inL.begin() + static_cast<long>(settled)));
    REQUIRE(std::equal(b.r.begin() + static_cast<long>(settled), b.r.end(), inR.begin() + static_cast<long>(settled)));
}

TEST_CASE("Reverb: pre-delay shifts the wet signal by exactly the set time", "[dsp][reverb]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    CAPTURE(fs);
    Reverb::Params p;
    p.mixPct = 100.0f;
    p.modPct = 0.0f; // the LFO is not shift-invariant; without it the whole wet path is LTI
    const auto n = static_cast<std::size_t>(0.6 * fs);
    std::vector<float> in(n, 0.0f);
    in[0] = 1.0f;
    p.preDelayMs = 0.0f;
    Reverb r0 = makeReverb(fs, p);
    const auto y0 = render(r0, in, in, 64);
    p.preDelayMs = 50.0f;
    Reverb r50 = makeReverb(fs, p);
    const auto y50 = render(r50, in, in, 64);
    const auto shift = static_cast<std::size_t>(std::lround(0.050 * fs));
    double err = 0.0, ref = 0.0;
    for (std::size_t i = shift; i < n; ++i) {
        err += std::pow(double(y50.l[i]) - y0.l[i - shift], 2.0);
        ref += double(y0.l[i - shift]) * y0.l[i - shift];
    }
    CAPTURE(10.0 * std::log10(err / ref));
    REQUIRE(err <= ref * 1e-10); // <= -100 dB
    REQUIRE(energy(y50.l, 0, shift) == 0.0); // nothing before the pre-delay
}

TEST_CASE("Reverb: mix steps leave <= -60 dBc residual against the linear-ramp law (REQ-TEST-003)",
          "[dsp][reverb][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 96000.0);
    const int block = GENERATE(37, 64, 512);
    const bool up = GENERATE(true, false);
    CAPTURE(fs, block, up);
    const auto tone = osp::measure::coherentTone(1000.0, fs);
    const int n = static_cast<int>(0.4 * fs);
    const auto in = osp::measure::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const int step = n / 2 + 7;                           // a non-block-aligned step ...
    const int at = (step + block - 1) / block * block;    // ... delivered at the next block boundary
    const float m0 = up ? 0.0f : 100.0f, m1 = up ? 100.0f : 0.0f;
    Reverb::Params p;
    p.modPct = 0.0f; // with the LFO off the wet path is LTI and independent of the mix, so the reference is exact
    // The wet signal alone: the same input at mix 100 (dry weight 0).
    p.mixPct = 100.0f;
    Reverb wetOnly = makeReverb(fs, p);
    const auto wet = render(wetOnly, in, in, block);
    p.mixPct = m0;
    Reverb r = makeReverb(fs, p);
    const auto out = render(r, in, in, block, [&](Reverb& rv, int pos) {
        if (pos == at) {
            Reverb::Params q = p;
            q.mixPct = m1;
            rv.setParameters(q);
        }
    });
    // Reference: out = dry·(1 − m) + wet·m, m a 20 ms linear ramp from the delivery sample (parameters.md §1.4).
    const int len = static_cast<int>(std::lround(0.020 * fs));
    std::vector<double> ref(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double a = m0 / 100.0, b = m1 / 100.0;
        const double m = i < at ? a : a + (b - a) * std::min(1.0, static_cast<double>(i - at + 1) / len);
        const auto k = static_cast<std::size_t>(i);
        ref[k] = double(in[k]) * (1.0 - m) + double(wet.l[k]) * m;
    }
    const double residual = osp::measure::residualDbc(out.l, ref);
    CAPTURE(residual);
    if (block == 64 && fs == 48000.0)
        WARN("REQ-TEST-003 reverb mix residual (" << (up ? "0 -> 100" : "100 -> 0") << " %): " << residual << " dBc");
    REQUIRE(residual <= -60.0);
}

TEST_CASE("Reverb: low and high cut shape the wet signal", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const auto n = static_cast<std::size_t>(1.0 * fs);
    const auto in = noise(n, 5u);
    auto bandEnergy = [&](float lowCut, float highCut, double f) {
        Reverb::Params p;
        p.mixPct = 100.0f;
        p.lowCutHz = lowCut;
        p.highCutHz = highCut;
        Reverb r = makeReverb(fs, p);
        const auto y = render(r, in, in, 64);
        // DFT power around f over the last 16384 samples (steady state), summed over +-5 bins.
        constexpr std::size_t kFft = 16384;
        std::vector<double> seg(y.l.end() - static_cast<long>(kFft), y.l.end());
        const auto spec = osp::measure::realFft(seg);
        const auto bin = static_cast<std::size_t>(f * double(seg.size()) / fs);
        double e = 0.0;
        for (std::size_t k = bin - 5; k <= bin + 5; ++k)
            e += std::norm(spec[k]);
        return e;
    };
    const double low100 = bandEnergy(50.0f, 10000.0f, 100.0), cut100 = bandEnergy(700.0f, 10000.0f, 100.0);
    const double high8k = bandEnergy(50.0f, 10000.0f, 8000.0), cut8k = bandEnergy(50.0f, 1000.0f, 8000.0);
    const double dbLow = osp::measure::powerToDb(cut100 / low100), dbHigh = osp::measure::powerToDb(cut8k / high8k);
    WARN("input cuts: low cut 50 -> 700 Hz changes 100 Hz by " << dbLow << " dB; high cut 10k -> 1k changes 8 kHz by "
                                                               << dbHigh << " dB");
    REQUIRE(dbLow <= -12.0); // one-pole HP: 700 Hz is ~2.8 octaves above 100 Hz
    REQUIRE(dbHigh <= -12.0);
}

TEST_CASE("Reverb: disabled keeps the trails and lets no new input into the tank", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const auto n = static_cast<std::size_t>(1.0 * fs);
    auto burst = noise(n, 6u);
    std::fill(burst.begin() + static_cast<long>(n / 4), burst.end(), 0.0f); // 0.25 s burst, then silence
    auto later = burst;
    const auto more = noise(n / 4, 7u);
    std::copy(more.begin(), more.end(), later.begin() + static_cast<long>(n / 2)); // input again at 0.5 s

    const auto offAt = static_cast<int>(n / 4) / 64 * 64;
    auto disableAt = [&](Reverb& r, int pos) {
        if (pos == offAt)
            r.setEnabled(false);
    };
    Reverb a = makeReverb(fs), b = makeReverb(fs);
    const auto ya = render(a, burst, burst, 64, disableAt);
    const auto yb = render(b, later, later, 64, disableAt);
    // After the 10 ms enable fade the output is dry + the tail of the burst: the input at 0.5 s only adds its dry part.
    const auto from = static_cast<std::size_t>(offAt) + static_cast<std::size_t>(0.011 * fs);
    double worst = 0.0;
    for (std::size_t i = from; i < n; ++i)
        worst = std::max(worst, std::abs(double(yb.l[i]) - later[i] - (double(ya.l[i]) - burst[i])));
    REQUIRE(worst <= 1e-6);
    REQUIRE(energy(ya.l, n / 2, 3 * n / 4) > 0.0); // the trail is still ringing at 0.5-0.75 s
}

TEST_CASE("Reverb: a mode change during a mode fade waits, so the audible engine is never reset", "[dsp][reverb]")
{
    const double fs = 48000.0;
    const auto n = static_cast<std::size_t>(0.5 * fs);
    auto in = noise(n, 8u);
    std::fill(in.begin() + static_cast<long>(n / 4), in.end(), 0.0f);
    const int switchAt = 6400, backAt = 6400 + 512; // back to room 10.7 ms into the 50 ms fade
    auto modes = [&](bool reverse) {
        return [=](Reverb& r, int pos) {
            Reverb::Params p;
            p.mode = Reverb::Mode::Room;
            if (pos >= switchAt && !(reverse && pos >= backAt))
                p.mode = Reverb::Mode::Hall;
            r.setParameters(p);
        };
    };
    Reverb::Params room;
    room.mode = Reverb::Mode::Room;
    Reverb a = makeReverb(fs, room), b = makeReverb(fs, room);
    const auto once = render(a, in, in, 64, modes(false));
    const auto back = render(b, in, in, 64, modes(true));
    // Identical until the first fade has finished: the reversal is deferred, not applied mid-fade.
    const auto fadeEnd = static_cast<std::size_t>(switchAt) + static_cast<std::size_t>(0.050 * fs);
    REQUIRE(std::equal(back.l.begin(), back.l.begin() + static_cast<long>(fadeEnd), once.l.begin()));
    REQUIRE_FALSE(std::equal(back.l.begin(), back.l.end(), once.l.begin())); // and then it does go back to room
}

TEST_CASE("Reverb: RT checker, 1000 random parameter vectors with NaN, full-scale noise", "[dsp][reverb][rt]")
{
    const double fs = 48000.0;
    Reverb r = makeReverb(fs);
    std::mt19937 rng(72u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto maybeNan = [&](float v) { return u(rng) < 0.02f ? nan : v; };
    std::vector<float> l(512), rr(512), ol(512), orr(512), all;
    all.reserve(1000 * 512);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int v = 0; v < 1000; ++v) {
            Reverb::Params p;
            p.mode = static_cast<Reverb::Mode>(static_cast<int>(u(rng) * 2.999f));
            p.decayS = maybeNan(-5.0f + 40.0f * u(rng));
            p.preDelayMs = maybeNan(-20.0f + 260.0f * u(rng));
            p.mixPct = maybeNan(-10.0f + 120.0f * u(rng));
            p.lowCutHz = maybeNan(10.0f + 900.0f * u(rng));
            p.highCutHz = maybeNan(500.0f + 12000.0f * u(rng));
            p.modPct = maybeNan(-10.0f + 120.0f * u(rng));
            p.freeze = u(rng) < 0.05f;
            r.setParameters(p);
            r.setEnabled(u(rng) > 0.1f);
            r.setHardBypass(u(rng) < 0.05f);
            for (std::size_t i = 0; i < l.size(); ++i) {
                l[i] = 2.0f * u(rng) - 1.0f;
                rr[i] = 2.0f * u(rng) - 1.0f;
            }
            const int n = 1 + static_cast<int>(u(rng) * 511.0f);
            r.process(l.data(), rr.data(), ol.data(), orr.data(), n);
            all.insert(all.end(), ol.begin(), ol.begin() + n);
            all.insert(all.end(), orr.begin(), orr.begin() + n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(all);
}
