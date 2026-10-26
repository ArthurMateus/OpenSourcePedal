// Tests for the noise gate (issue #65, parameters.md §2.2).

#include "osp/dsp/Gate.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace {

using osp::dsp::Gate;
constexpr int kMaxBlock = 2048;

float db(float d)
{
    return std::pow(10.0f, d / 20.0f);
}

// Sine at `levelDb` peak, frequency f, for `seconds`.
std::vector<float> tone(double fs, double f, float levelDb, double seconds)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * fs));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = db(levelDb) * static_cast<float>(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / fs));
    return x;
}

Gate makeGate(double fs, const Gate::Params& p = {})
{
    Gate g;
    g.prepare(fs, kMaxBlock);
    g.setParameters(p);
    g.setEnabled(true);
    g.reset(); // settle smoothers and the enable crossfade on the new values
    return g;
}

// Renders through process() in blocks of `blockSize` (0 = an empty call before each 64-sample block), under the RT
// checker, and scans the output.
std::vector<float> render(Gate& g, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (blockSize == 0)
                g.process(in.data() + pos, out.data() + pos, 0);
            g.process(in.data() + pos, out.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

// Per-sample gain trace from the key, one sample per call so the open state can be observed.
struct Trace {
    std::vector<float> gain;
    int openings = 0, closings = 0;
};
Trace trace(Gate& g, const std::vector<float>& key)
{
    Trace t;
    t.gain.resize(key.size());
    bool wasOpen = g.isOpen();
    for (std::size_t i = 0; i < key.size(); ++i) {
        g.computeGain(&key[i], &t.gain[i], 1);
        if (g.isOpen() != wasOpen)
            (g.isOpen() ? t.openings : t.closings)++;
        wasOpen = g.isOpen();
    }
    return t;
}

} // namespace

TEST_CASE("Gate: rate x block-size matrix, block-size invariant, pre-prepare", "[dsp][gate]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, blockSize);

    // Burst: 0.1 s silence, 0.2 s at −30 dB, 0.3 s silence.
    std::vector<float> in(static_cast<std::size_t>(0.6 * fs), 0.0f);
    const auto burst = tone(fs, 220.0, -30.0f, 0.2);
    std::copy(burst.begin(), burst.end(), in.begin() + static_cast<long>(0.1 * fs));

    Gate g = makeGate(fs);
    const auto out = render(g, in, blockSize);
    Gate ref = makeGate(fs);
    REQUIRE(out == render(ref, in, 64)); // identical for every block size

    // Open during the burst (unity gain after the 0.5 ms attack), closing after it: detector decay, 20 ms hold, then
    // the 80 ms release (more than 2.5 τ has passed at the end of the render).
    auto mid = static_cast<std::size_t>(0.2 * fs);
    while (std::abs(in[mid]) < 0.5f * db(-30.0f)) // a sample away from a zero crossing
        ++mid;
    REQUIRE(out[mid] == in[mid]);
    REQUIRE_FALSE(g.isOpen());
    REQUIRE(g.currentGain() < 0.1f);

    Gate unprepared; // before prepare(): zero output, no crash
    std::vector<float> z(16, 1.0f);
    unprepared.process(z.data(), z.data(), 16);
    REQUIRE(std::all_of(z.begin(), z.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Gate: hysteresis and hold state machine", "[dsp][gate]")
{
    const double fs = 48000.0;
    Gate::Params p;
    p.thresholdDb = -50.0f;
    p.hysteresisDb = 6.0f; // close below −56 dB
    p.holdMs = 20.0f;
    p.detector = Gate::Detector::Rms; // a steady sine gives a steady level
    Gate g = makeGate(fs, p);

    // RMS of a sine is −3 dB from its peak. Levels below are RMS values.
    auto level = [&](float rmsDb, double s) { return tone(fs, 1000.0, rmsDb + 3.0103f, s); };
    std::vector<float> key;
    for (auto [l, s] : {std::pair{-48.0f, 0.2}, std::pair{-53.0f, 0.3}, std::pair{-60.0f, 0.3}, std::pair{-53.0f, 0.3}}) {
        const auto seg = level(l, s);
        key.insert(key.end(), seg.begin(), seg.end());
    }
    const auto t = trace(g, key);
    // Opens at −48, stays open at −53 (inside the hysteresis band), closes at −60, stays closed at −53 (below −50).
    REQUIRE(t.openings == 1);
    REQUIRE(t.closings == 1);
    REQUIRE_FALSE(g.isOpen());

    // Hold: after the level falls below the close threshold, the gain stays at 1 for the hold time.
    const auto dropAt = static_cast<std::size_t>(0.5 * fs); // start of the −60 dB segment
    const auto holdEnd = dropAt + static_cast<std::size_t>(0.020 * fs);
    REQUIRE(t.gain[dropAt + static_cast<std::size_t>(0.012 * fs)] == 1.0f); // inside hold (RMS detector lag < 8 ms)
    REQUIRE(t.gain[holdEnd + static_cast<std::size_t>(0.030 * fs)] < 1.0f);
}

TEST_CASE("Gate: measured attack and release time constants, range floor", "[dsp][gate]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    Gate::Params p;
    p.attackMs = 2.0f;
    p.releaseMs = 50.0f;
    p.holdMs = 0.0f;
    p.rangeDb = -40.0f;
    Gate g = makeGate(fs, p);
    const float floor = db(-40.0f);
    REQUIRE(std::abs(g.currentGain() - floor) <= 1e-7f); // closed at the range floor

    // Key: 0.1 s of a loud DC-free square (peak detector sees full level at once), then silence.
    std::vector<float> key(static_cast<std::size_t>(0.6 * fs), 0.0f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.1 * fs); ++i)
        key[i] = (i / 8) % 2 ? 0.1f : -0.1f;
    const auto t = trace(g, key);

    // Attack: time to 63 % of the way from the floor to 1.
    const float attack63 = floor + (1.0f - floor) * (1.0f - std::exp(-1.0f));
    const auto a = std::find_if(t.gain.begin(), t.gain.end(), [&](float v) { return v >= attack63; }) - t.gain.begin();
    REQUIRE(std::abs(static_cast<double>(a) / fs - 0.002) <= 0.0002); // ±10 %

    // Release: from the moment the gate closes (the peak detector decays 10 ms, then the level drops below
    // threshold − hysteresis), the gain falls 63 % of the way to the floor in τ = 50 ms.
    std::size_t closeAt = static_cast<std::size_t>(0.1 * fs);
    while (t.gain[closeAt] == 1.0f)
        ++closeAt;
    const float release63 = 1.0f - (1.0f - floor) * (1.0f - std::exp(-1.0f));
    std::size_t r = closeAt;
    while (t.gain[r] > release63)
        ++r;
    REQUIRE(std::abs(static_cast<double>(r - closeAt + 1) / fs - 0.050) <= 0.005);
    REQUIRE(t.gain.back() <= floor * 1.05f); // settles on the floor
    WARN("gate timing fs=" << fs << ": attack63 " << 1e3 * double(a) / fs << " ms (spec 2), release63 "
                            << 1e3 * double(r - closeAt + 1) / fs << " ms (spec 50)");
}

TEST_CASE("Gate: no chatter on a decaying tone", "[dsp][gate]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const auto detector = GENERATE(Gate::Detector::Peak, Gate::Detector::Rms);
    Gate::Params p;
    p.detector = detector;
    Gate g = makeGate(fs, p); // defaults: −55 dB threshold, 4 dB hysteresis, 20 ms hold

    // Low E (82.4 Hz) decaying from −12 dBFS by 30 dB/s, plus −90 dBFS noise: it crosses the threshold slowly.
    std::vector<float> key(static_cast<std::size_t>(3.0 * fs));
    std::mt19937 rng(65u);
    std::normal_distribution<float> noise(0.0f, db(-90.0f));
    for (std::size_t i = 0; i < key.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        key[i] = db(static_cast<float>(-12.0 - 30.0 * t)) * static_cast<float>(std::sin(2.0 * std::numbers::pi * 82.4 * t)) +
                 noise(rng);
    }
    const auto tr = trace(g, key);
    REQUIRE(tr.openings == 1);
    REQUIRE(tr.closings == 1);
}

TEST_CASE("Gate: disabled is an exact copy (hard bypass null); range 0 dB passes; reset is bit-identical", "[dsp][gate]")
{
    const double fs = 48000.0;
    const auto in = tone(fs, 440.0, -70.0f, 0.5); // below threshold: the gate would close

    Gate off;
    off.prepare(fs, kMaxBlock); // disabled by default (parameters.md §1.6)
    REQUIRE(render(off, in, 64) == in);

    Gate::Params p;
    p.rangeDb = 0.0f;
    Gate open = makeGate(fs, p);
    REQUIRE(render(open, in, 64) == in);

    Gate g = makeGate(fs);
    const auto a = render(g, in, 64);
    g.reset();
    REQUIRE(render(g, in, 64) == a);
}

TEST_CASE("Gate: range smoothing residual vs per-sample reference (REQ-TEST-003)", "[dsp][gate][zipper]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int blockSize = GENERATE(32, 64, 127);
    const bool up = GENERATE(true, false);
    CAPTURE(fs, blockSize, up);

    // The gate is held closed (silent key) and its gain is applied to a 1 kHz sine through the post-amp tap. The
    // range steps between −60 and −20 dB at the first block boundary ≥ the (non-aligned) step sample, in the middle
    // of the record. Reference, in double and per sample: the range one-pole (20 ms), then the gain one-pole (attack
    // τ when rising, release τ when falling).
    Gate::Params p;
    p.rangeDb = up ? -60.0f : -20.0f;
    Gate g = makeGate(fs, p);
    const int step = static_cast<int>(0.8 * fs) + 1;
    const int n = 2 * step;
    const int at = (step + blockSize - 1) / blockSize * blockSize;
    std::vector<float> key(static_cast<std::size_t>(n), 0.0f), gain(key.size()), out(key.size());
    std::vector<double> ref(key.size());
    const auto sine = [&](int i) { return 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / fs); };
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < n; pos += blockSize) {
            if (pos == at) {
                p.rangeDb = up ? -20.0f : -60.0f;
                g.setParameters(p);
            }
            const int m = std::min(blockSize, n - pos);
            g.computeGain(key.data() + pos, gain.data() + pos, m);
            for (int i = pos; i < pos + m; ++i)
                out[static_cast<std::size_t>(i)] = static_cast<float>(sine(i));
            Gate::applyGain(gain.data() + pos, out.data() + pos, m);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);

    const double from = std::pow(10.0, (up ? -60.0 : -20.0) / 20.0), to = std::pow(10.0, (up ? -20.0 : -60.0) / 20.0);
    const double aSm = std::exp(-1.0 / (0.020 * fs)), aAtt = std::exp(-1.0 / (0.0005 * fs)),
                 aRel = std::exp(-1.0 / (0.080 * fs));
    double gref = from;
    for (int i = 0; i < n; ++i) {
        const double target = i < at ? from : to + (from - to) * std::pow(aSm, i - at + 1);
        gref = target + (target > gref ? aAtt : aRel) * (gref - target);
        ref[static_cast<std::size_t>(i)] = sine(i) * gref;
    }
    const double residual = osp::measure::residualDbc(out, ref);
    CAPTURE(residual);
    REQUIRE(residual <= -60.0);
    if (blockSize == 64)
        WARN("gate range residual fs=" << fs << (up ? " up " : " down ") << residual << " dBc");
}

TEST_CASE("Gate: open gate is linear (ADR-0007 alias-to-fundamental <= -80 dB)", "[dsp][gate]")
{
    const double fs = GENERATE(44100.0, 48000.0);
    const double f0 = GENERATE(1000.0, 5000.0);
    Gate::Params p;
    p.thresholdDb = -90.0f; // always open for a −12 dBFS tone
    Gate g = makeGate(fs, p);
    const auto tone = osp::measure::coherentTone(f0, fs);
    const auto in = osp::measure::makeTone(tone, fs, -12.0, osp::measure::kWarmupSamples + osp::measure::kCoherentFftSize);
    const auto out = render(g, in, 64);
    const auto r = osp::measure::analyseCoherent(out, tone.bin);
    CAPTURE(r.aliasToFundamentalDb, r.thd, r.fundamentalDbfs);
    REQUIRE(r.aliasToFundamentalDb <= -80.0);
    REQUIRE(std::abs(r.fundamentalDbfs + 12.0) <= 0.01);
    WARN("gate open fs=" << fs << " f0=" << f0 << ": alias-to-fund " << r.aliasToFundamentalDb << " dB, THD " << r.thd);
}

TEST_CASE("Gate: RT checker, 1000 random parameter vectors, impulse + 10 s silence", "[dsp][gate][rt]")
{
    constexpr double fs = 48000.0;
    constexpr int kBlock = 64;
    Gate g = makeGate(fs);
    std::vector<float> in(static_cast<std::size_t>(fs * 10.5), 0.0f), out(in.size());
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.5 * fs); ++i)
        in[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i))) * (i == 0 ? 2.0f : 1.0f);

    std::mt19937 rng(650u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<Gate::Params> params(1000);
    for (auto& q : params) {
        // Deliberately up to 20 % outside every range: setParameters() must clamp.
        q.thresholdDb = -100.0f + 90.0f * u(rng);
        q.hysteresisDb = -2.0f + 14.0f * u(rng);
        q.attackMs = 12.0f * u(rng);
        q.holdMs = -20.0f + 240.0f * u(rng);
        q.releaseMs = 600.0f * u(rng);
        q.rangeDb = -100.0f + 110.0f * u(rng);
        q.detector = u(rng) < 0.5f ? Gate::Detector::Peak : Gate::Detector::Rms;
    }
    std::vector<int> enables(1000);
    for (auto& e : enables)
        e = u(rng) < 0.8f;

    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0, b = 0; pos < total; pos += kBlock, ++b) {
            if (b < 1000) {
                g.setParameters(params[static_cast<std::size_t>(b)]);
                g.setEnabled(enables[static_cast<std::size_t>(b)] != 0);
            }
            g.process(in.data() + pos, out.data() + pos, std::min(kBlock, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
}
