#include "osp/dsp/amp/ToneStack.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

using namespace osp::dsp::amp;
namespace ms = osp::measure;

namespace {

constexpr double kPi = 3.14159265358979323846;
using Complex = std::complex<double>;
constexpr std::array<ToneStackVariant, 3> kVariants{ToneStackVariant::a, ToneStackVariant::b, ToneStackVariant::c};
constexpr std::array<float, 5> kGrid{0.0f, 2.5f, 5.0f, 7.5f, 10.0f};

struct Knobs {
    double bass = 5.0, mid = 5.0, treble = 5.0;
};

// Independent reference: complex nodal analysis of the netlist in ToneStack.h at s = j 2 pi f (Gaussian elimination
// with partial pivoting; a 0-ohm pot leg is 1 micro-ohm).
Complex network(const ToneStackComponents& k, const Knobs& kn, double hz)
{
    const double t = toneStackPotFraction(kn.treble, k.trebleAudio, false);
    const double l = toneStackPotFraction(kn.bass, k.bassAudio, true);
    const double m = toneStackPotFraction(kn.mid, k.midAudio, true);
    const Complex s(0.0, 2.0 * kPi * hz);
    std::array<std::array<Complex, 6>, 6> a{};
    std::array<Complex, 6> r{};
    auto g = [](double ohms) { return Complex(1.0 / std::max(ohms, 1e-6), 0.0); };
    auto stamp = [&](int i, int j, Complex y) { // nodes V0 V1 Vo V2 V3 V4 = 0..5, ground = -1
        if (i >= 0)
            a[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] += y;
        if (j >= 0)
            a[static_cast<std::size_t>(j)][static_cast<std::size_t>(j)] += y;
        if (i >= 0 && j >= 0) {
            a[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] -= y;
            a[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] -= y;
        }
    };
    stamp(0, -1, g(k.rSource));
    r[0] = g(k.rSource); // Vin = 1 behind Rs
    stamp(0, 1, s * k.cTreble);
    stamp(1, 2, g((1.0 - t) * k.rTreble));
    stamp(2, 3, g(t * k.rTreble));
    stamp(2, -1, g(k.rLoad));
    stamp(0, 4, g(k.rSlope));
    stamp(4, 3, s * k.cBass);
    stamp(4, 5, s * k.cMid);
    stamp(3, 5, g(l * k.rBass));
    stamp(5, -1, g(m * k.rMid));
    for (std::size_t c = 0; c < 6; ++c) {
        std::size_t piv = c;
        for (std::size_t i = c + 1; i < 6; ++i)
            if (std::abs(a[i][c]) > std::abs(a[piv][c]))
                piv = i;
        std::swap(a[c], a[piv]);
        std::swap(r[c], r[piv]);
        for (std::size_t i = c + 1; i < 6; ++i) {
            const Complex f = a[i][c] / a[c][c];
            for (std::size_t j = c; j < 6; ++j)
                a[i][j] -= f * a[c][j];
            r[i] -= f * r[c];
        }
    }
    std::array<Complex, 6> v{};
    for (int i = 5; i >= 0; --i) {
        const auto u = static_cast<std::size_t>(i);
        Complex acc = r[u];
        for (std::size_t j = u + 1; j < 6; ++j)
            acc -= a[u][j] * v[j];
        v[u] = acc / a[u][u];
    }
    return v[2];
}

Complex digital(const ToneStackCoeffs& c, double hz, double fs)
{
    const Complex e = std::exp(Complex(0.0, -2.0 * kPi * hz / fs));
    Complex h(c.d, 0.0);
    for (std::size_t i = 0; i < 3; ++i)
        h += c.r[i] * e / (1.0 - c.z[i] * e);
    return h;
}

double db(Complex h)
{
    return ms::toDb(std::abs(h));
}

ToneStack makeStack(ToneStackVariant v, const Knobs& kn, double fs)
{
    ToneStack ts;
    ts.setVariant(v);
    ts.setBass(static_cast<float>(kn.bass));
    ts.setMid(static_cast<float>(kn.mid));
    ts.setTreble(static_cast<float>(kn.treble));
    ts.prepare(fs, 2048);
    return ts;
}

std::vector<float> noise(int n, std::uint32_t seed, float scale = 0.5f)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = scale * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// Renders `in` in blocks under the RT checker; `perBlock(pos)` runs before each block. Block size 0 adds a no-op call.
template <typename PerBlock>
std::vector<float> render(ToneStack& ts, const std::vector<float>& in, int blockSize, PerBlock&& perBlock)
{
    std::vector<float> out = in;
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            perBlock(pos);
            if (blockSize == 0)
                ts.process(out.data() + pos, 0);
            ts.process(out.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}

std::vector<float> render(ToneStack& ts, const std::vector<float>& in, int blockSize)
{
    return render(ts, in, blockSize, [](int) {});
}

// Double-precision reference realisation of a per-sample coefficient sequence (the same modal form as ToneStack).
struct RefModal {
    double x1 = 0.0;
    std::array<double, 3> w{};
    double tick(const ToneStackCoeffs& c, double in)
    {
        double y = c.d * in;
        for (std::size_t i = 0; i < 3; ++i) {
            w[i] = x1 + c.z[i] * w[i];
            y += c.r[i] * w[i];
        }
        x1 = in;
        return y;
    }
};

} // namespace

TEST_CASE("ToneStack: closed-form analog coefficients match an independent nodal solve", "[dsp][amp][tonestack]")
{
    std::uint32_t seed = 7u;
    auto uni = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    double worst = 0.0;
    for (ToneStackVariant v : kVariants)
        for (int i = 0; i < 200; ++i) {
            const auto& k = toneStackComponents(v);
            const Knobs kn{10.0 * uni(), 10.0 * uni(), 10.0 * uni()};
            const double hz = 20.0 * std::pow(1000.0, uni());
            const auto h = toneStackAnalog(k, toneStackPotFraction(kn.treble, k.trebleAudio, false),
                                           toneStackPotFraction(kn.bass, k.bassAudio, true),
                                           toneStackPotFraction(kn.mid, k.midAudio, true));
            const Complex s(0.0, 2.0 * kPi * hz);
            const Complex hs = (((h.b[3] * s + h.b[2]) * s + h.b[1]) * s + h.b[0]) /
                               (((h.a[3] * s + h.a[2]) * s + h.a[1]) * s + h.a[0]);
            const Complex ref = network(k, kn, hz);
            worst = std::max(worst, std::abs(hs - ref) / std::abs(ref));
        }
    CAPTURE(worst);
    REQUIRE(worst < 1e-9);
}

TEST_CASE("ToneStack: magnitude within 0.1 dB of the analog network on the knob grid; poles inside the unit circle",
          "[dsp][amp][tonestack]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0, 768000.0);
    CAPTURE(fs);
    const double top = fs < 80000.0 ? 16000.0 : 20000.0; // 20 kHz from 88.2 kHz up (the fit band)
    std::vector<double> freqs;
    for (int i = 0; i < 64; ++i)
        freqs.push_back(20.0 * std::pow(top / 20.0, i / 63.0));
    double worst = 0.0, zMax = 0.0;
    for (ToneStackVariant v : kVariants)
        for (float b : kGrid)
            for (float mi : kGrid)
                for (float t : kGrid) {
                    const Knobs kn{b, mi, t};
                    auto ts = makeStack(v, kn, fs);
                    float x = 0.0f;
                    ts.process(&x, 1);
                    const auto& c = ts.coefficients();
                    REQUIRE(ts.designFailures() == 0);
                    for (double z : c.z) {
                        REQUIRE(std::isfinite(z));
                        REQUIRE(z >= 0.0);
                        REQUIRE(z < 1.0);
                        zMax = std::max(zMax, z);
                    }
                    for (double f : freqs) {
                        const double e = std::abs(db(digital(c, f, fs)) - db(network(toneStackComponents(v), kn, f)));
                        if (e > worst) {
                            worst = e;
                            CAPTURE(static_cast<int>(v), b, mi, t, f);
                            REQUIRE(e <= 0.1);
                        }
                    }
                }
    WARN("tone stack grid error at " << fs << " Hz: max " << worst << " dB over 20 Hz.." << top << " Hz, largest pole "
                                     << zMax);
}

TEST_CASE("ToneStack: process() realises the design (impulse response vs analog network)", "[dsp][amp][tonestack]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    const auto v = GENERATE(ToneStackVariant::a, ToneStackVariant::b);
    CAPTURE(fs, static_cast<int>(v));
    constexpr std::size_t kLen = 1u << 17;
    for (const Knobs kn :
         {Knobs{}, Knobs{0, 0, 0}, Knobs{10, 10, 10}, Knobs{10, 0, 0}, Knobs{0, 10, 0}, Knobs{0, 0, 10}}) {
        auto ts = makeStack(v, kn, fs);
        std::vector<float> impulse(kLen, 0.0f);
        impulse[0] = 1.0f;
        const auto ir = render(ts, impulse, 64);
        OSP_REQUIRE_OUTPUT_CLEAN(ir);
        const auto spec = ms::realFft(std::vector<double>(ir.begin(), ir.end()));
        for (int i = 0; i < 40; ++i) {
            const double want = 20.0 * std::pow(800.0, i / 39.0); // 20 Hz .. 16 kHz
            const auto bin = static_cast<std::size_t>(std::lround(want * kLen / fs));
            const double f = static_cast<double>(bin) * fs / kLen;
            const double e = std::abs(db(spec[bin]) - db(network(toneStackComponents(v), kn, f)));
            CAPTURE(kn.bass, kn.mid, kn.treble, f);
            REQUIRE(e <= 0.1);
        }
    }
}

TEST_CASE("ToneStack: knob ranges and insertion loss (gain staging)", "[dsp][amp][tonestack]")
{
    const double fs = 192000.0;
    for (ToneStackVariant v : kVariants) {
        const auto& k = toneStackComponents(v);
        auto at = [&](Knobs kn, double hz) { return db(network(k, kn, hz)); };
        const double mid1k = at({}, 1000.0);
        const double bassRange = at({10, 5, 5}, 60.0) - at({0, 5, 5}, 60.0);
        const double trebleRange = at({5, 5, 10}, 5000.0) - at({5, 5, 0}, 5000.0);
        const double midRange = at({5, 10, 5}, 500.0) - at({5, 0, 5}, 500.0);
        double peak = -1e9;
        for (float b : kGrid)
            for (float mi : kGrid)
                for (float t : kGrid)
                    for (int i = 0; i < 40; ++i)
                        peak = std::max(peak, at({b, mi, t}, 20.0 * std::pow(1000.0, i / 39.0)));
        CAPTURE(static_cast<int>(v), mid1k, bassRange, trebleRange, midRange, peak);
        REQUIRE(mid1k <= -3.0); // a passive stack always loses level at mid settings
        REQUIRE(mid1k >= -30.0);
        REQUIRE(bassRange >= 6.0);
        REQUIRE(trebleRange >= 6.0);
        REQUIRE(midRange >= 3.0);
        REQUIRE(peak <= 0.5); // passive: no gain
        WARN("variant " << static_cast<int>(v) << ": 1 kHz at knobs 5 = " << mid1k << " dB; bass 0->10 at 60 Hz "
                        << bassRange << " dB, treble 0->10 at 5 kHz " << trebleRange << " dB, mid 0->10 at 500 Hz "
                        << midRange << " dB; peak over grid " << peak << " dB");
        (void)fs;
    }
}

TEST_CASE("ToneStack: knob steps follow the 20 ms law, <= -60 dBc residual (REQ-TEST-003)",
          "[dsp][amp][tonestack][zipper]")
{
    const double fs = GENERATE(44100.0, 192000.0);
    const int knob = GENERATE(0, 1, 2); // bass, mid, treble
    const bool up = GENERATE(true, false);
    CAPTURE(fs, knob, up);
    const auto tone = ms::coherentTone(1000.0, fs);
    const int n = static_cast<int>(0.4 * fs);
    const auto in = ms::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const double from = up ? 0.0 : 10.0, to = up ? 10.0 : 0.0;
    auto knobsAt = [&](double value) {
        Knobs kn;
        (knob == 0 ? kn.bass : knob == 1 ? kn.mid : kn.treble) = value;
        return kn;
    };

    // Reference: the knob follows the one-pole law per sample from the delivery sample, designed every sample.
    ToneStackDesigner designer;
    designer.prepare(fs);
    const auto& k = kToneStackB;
    const double a = std::exp(-1.0 / (ToneStack::kSmoothingSeconds * fs));
    for (const int block : {37, 512}) {
        CAPTURE(block);
        const int at = (n / 2 + block - 1) / block * block;
        std::vector<double> ref(static_cast<std::size_t>(n));
        RefModal cascade;
        ToneStackCoeffs c;
        const Knobs k0 = knobsAt(from);
        REQUIRE(designer.design(k, k0.bass, k0.mid, k0.treble, c));
        double dev = from - to;
        for (int i = 0; i < n; ++i) {
            if (i >= at && dev != 0.0) {
                dev *= a;
                if (std::abs(dev) < 1e-6)
                    dev = 0.0;
                const Knobs kn = knobsAt(to + dev);
                REQUIRE(designer.design(k, kn.bass, kn.mid, kn.treble, c));
            }
            ref[static_cast<std::size_t>(i)] = cascade.tick(c, in[static_cast<std::size_t>(i)]);
        }
        ToneStack ts;
        ts.setVariant(ToneStackVariant::b);
        const Knobs kStart = knobsAt(from);
        ts.setBass(static_cast<float>(kStart.bass));
        ts.setMid(static_cast<float>(kStart.mid));
        ts.setTreble(static_cast<float>(kStart.treble));
        ts.prepare(fs, 512);
        const auto out = render(ts, in, block, [&](int pos) {
            if (pos == at) {
                const Knobs kn = knobsAt(to);
                ts.setBass(static_cast<float>(kn.bass));
                ts.setMid(static_cast<float>(kn.mid));
                ts.setTreble(static_cast<float>(kn.treble));
            }
        });
        OSP_REQUIRE_OUTPUT_CLEAN(out);
        const double r = ms::residualDbc(out, ref);
        CAPTURE(r);
        REQUIRE(r <= -60.0);
        if (block == 512)
            WARN("REQ-TEST-003 tone stack residual at " << fs << " Hz, knob " << knob << (up ? " up: " : " down: ") << r
                                                        << " dBc");
    }
}

TEST_CASE("ToneStack: an automated sweep of all three knobs has no zipper residual", "[dsp][amp][tonestack][zipper]")
{
    const double fs = GENERATE(48000.0, 192000.0);
    const int block = fs > 100000.0 ? 256 : 64;
    CAPTURE(fs, block);
    const int n = static_cast<int>(0.3 * fs);
    const auto tone = ms::coherentTone(1000.0, fs);
    auto in = ms::makeTone(tone, fs, -12.0, static_cast<std::size_t>(n));
    const auto hiss = noise(n, 3u, 0.05f); // broadband content shows any HF zipper too
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] += hiss[i];
    // Host automation: new targets every block, all knobs moving across 0.2 s (the last 0.1 s settles).
    auto targets = [&](int pos) {
        const double x = std::min(1.0, pos / (0.2 * fs));
        return Knobs{10.0 * x, 10.0 * (1.0 - x), 5.0 + 5.0 * std::sin(2.0 * kPi * x)};
    };
    ToneStackDesigner designer;
    designer.prepare(fs);
    const double a = std::exp(-1.0 / (ToneStack::kSmoothingSeconds * fs));
    std::vector<double> ref(static_cast<std::size_t>(n));
    RefModal cascade;
    Knobs cur = targets(0), tgt = cur;
    ToneStackCoeffs c;
    for (int i = 0; i < n; ++i) {
        if (i % block == 0)
            tgt = targets(i);
        cur = {tgt.bass + (cur.bass - tgt.bass) * a, tgt.mid + (cur.mid - tgt.mid) * a,
               tgt.treble + (cur.treble - tgt.treble) * a};
        REQUIRE(designer.design(kToneStackA, cur.bass, cur.mid, cur.treble, c));
        ref[static_cast<std::size_t>(i)] = cascade.tick(c, in[static_cast<std::size_t>(i)]);
    }
    auto ts = makeStack(ToneStackVariant::a, targets(0), fs);
    const auto out = render(ts, in, block, [&](int pos) {
        const Knobs kn = targets(pos);
        ts.setBass(static_cast<float>(kn.bass));
        ts.setMid(static_cast<float>(kn.mid));
        ts.setTreble(static_cast<float>(kn.treble));
    });
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    // Whole-band residual, and the residual above 4 kHz (where per-block steps would show) relative to the output.
    const double r = ms::residualDbc(out, ref);
    std::vector<double> diff(1u << 13), sig(1u << 13); // n / 6 + 8192 <= n at both rates
    const std::size_t from = static_cast<std::size_t>(n / 6);
    for (std::size_t i = 0; i < diff.size(); ++i) {
        diff[i] = out[from + i] - ref[from + i];
        sig[i] = ref[from + i];
    }
    const auto pd = ms::powerSpectrum(diff), ps = ms::powerSpectrum(sig);
    double eDiff = 0.0, eSig = 0.0;
    for (std::size_t b = 0; b < pd.size(); ++b) {
        eSig += ps[b];
        if (static_cast<double>(b) * fs / static_cast<double>(diff.size()) > 4000.0)
            eDiff += pd[b];
    }
    const double hf = ms::powerToDb(eDiff / eSig);
    CAPTURE(r, hf);
    REQUIRE(r <= -60.0);
    REQUIRE(hf <= -80.0);
    WARN("tone stack sweep at " << fs << " Hz / " << block << ": residual " << r << " dBc, HF residual " << hf
                                << " dB");
}

TEST_CASE("ToneStack: rate x block-size matrix, knob fuzz, RT clean, reset() determinism", "[dsp][amp][tonestack]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    const int n = static_cast<int>(0.15 * fs);
    auto in = noise(n, 11u, 1.0f);
    std::fill(in.begin() + n / 2, in.begin() + n / 2 + n / 8, 0.0f); // a silent stretch: no subnormal tails
    const float specials[] = {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              -3.0f,
                              25.0f,
                              0.0f,
                              10.0f};
    ToneStack ts;
    ts.setVariant(block % 2 ? ToneStackVariant::b : ToneStackVariant::a);
    ts.prepare(fs, 2048);
    auto fuzz = [&](std::uint32_t seed) {
        return [&ts, &specials, seed, next = 0, every = static_cast<int>(0.005 * fs)](int pos) mutable {
            if (pos < next)
                return; // new targets about every 5 ms of audio (every block for large blocks)
            next = pos + every;
            for (int k = 0; k < 3; ++k) {
                seed = seed * 1664525u + 1013904223u;
                const float v = (seed >> 28) == 0 ? specials[(seed >> 8) % 7]
                                                  : 10.0f * (static_cast<float>(seed >> 8) / 16777216.0f);
                if (k == 0)
                    ts.setBass(v);
                else if (k == 1)
                    ts.setMid(v);
                else
                    ts.setTreble(v);
            }
        };
    };
    const auto a = render(ts, in, block, fuzz(5u));
    OSP_REQUIRE_OUTPUT_CLEAN(a);
    REQUIRE(ts.designFailures() == 0);
    ts.setBass(5.0f);
    ts.setMid(5.0f);
    ts.setTreble(5.0f);
    ts.reset();
    const auto b = render(ts, in, block, fuzz(5u));
    ts.setBass(5.0f);
    ts.setMid(5.0f);
    ts.setTreble(5.0f);
    ts.reset();
    const auto c = render(ts, in, block, fuzz(5u));
    REQUIRE(b == c); // reset() makes repeat renders bit-identical
    REQUIRE(ts.getLatencySamples() == 0);
}

TEST_CASE("ToneStack: a static setting gives identical output for every block size", "[dsp][amp][tonestack]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const auto in = noise(static_cast<int>(0.1 * fs), 21u);
    auto ts = makeStack(ToneStackVariant::b, {2.0, 7.0, 9.0}, fs);
    const auto ref = render(ts, in, 64);
    for (int block : {1, 32, 127, 512, 2048}) {
        ts.reset();
        CAPTURE(block);
        REQUIRE(render(ts, in, block) == ref);
    }
}

TEST_CASE("ToneStack: linear (THD), recovers from non-finite input, zero-fills before prepare()",
          "[dsp][amp][tonestack]")
{
    const double fs = 48000.0;
    const auto tone = ms::coherentTone(1000.0, fs);
    for (double level : {-6.0, 18.0}) {
        auto ts = makeStack(ToneStackVariant::a, {}, fs);
        const auto in = ms::makeTone(tone, fs, level, ms::kCoherentFftSize + ms::kWarmupSamples);
        const auto out = render(ts, in, 64);
        const auto rep = ms::analyseCoherent(out, tone.bin);
        CAPTURE(level, rep.thdN);
        REQUIRE(rep.thdN <= 1e-5); // a linear filter: only float output rounding (< -100 dB)
    }

    auto ts = makeStack(ToneStackVariant::c, {}, fs);
    std::vector<float> buf(256, 0.25f);
    buf[10] = std::numeric_limits<float>::quiet_NaN();
    (void)render(ts, buf, 256);
    std::vector<float> after = noise(512, 4u);
    after = render(ts, after, 64);
    OSP_REQUIRE_OUTPUT_CLEAN(after);

    ToneStack unprepared;
    std::vector<float> x(64, 0.5f);
    unprepared.process(x.data(), 64);
    REQUIRE(std::all_of(x.begin(), x.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("ToneStack: CPU per sample (informative)", "[.][perf][amp][tonestack]")
{
    for (const double fs : {48000.0, 192000.0}) {
        const int block = static_cast<int>(64 * fs / 48000.0);
        auto in = noise(block, 9u);
        for (const bool sweep : {false, true}) {
            auto ts = makeStack(ToneStackVariant::b, {}, fs);
            const int blocks = 20000;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < blocks; ++i) {
                if (sweep) {
                    const float v = static_cast<float>(i % 400) / 40.0f;
                    ts.setBass(v);
                    ts.setTreble(10.0f - v);
                }
                ts.process(in.data(), block);
            }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double ns = 1e9 * sec / (static_cast<double>(blocks) * block);
            WARN("tone stack " << fs << " Hz, block " << block << (sweep ? ", knobs moving: " : ", static: ") << ns
                               << " ns/sample = " << 100.0 * ns * 1e-9 * fs << " % of one core");
        }
    }
}
