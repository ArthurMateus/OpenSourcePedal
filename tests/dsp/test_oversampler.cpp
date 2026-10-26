#include "measure.hpp"
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
#include <vector>

using osp::dsp::Oversampler;

namespace {

// ADR-0005 D3, IIR (Live): {up dB, up transition, down dB, down transition} per stage (0 = fs <-> 2fs).
struct StageSpec {
    double upDb, upTw, downDb, downTw;
};
constexpr StageSpec kSpecs[] = {{90, 0.05, 75, 0.06}, {100, 0.10, 85, 0.12}, {110, 0.10, 95, 0.12}};

std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    return x;
}

// |X[b]| in dB relative to |X[0]| (the DC gain), for a real impulse response.
std::vector<double> magnitudeDb(const std::vector<float>& h)
{
    const std::vector<double> x(h.begin(), h.end());
    const auto spectrum = osp::measure::realFft(x);
    std::vector<double> db(spectrum.size());
    for (std::size_t b = 0; b < db.size(); ++b)
        db[b] = osp::measure::toDb(std::abs(spectrum[b]) / std::abs(spectrum[0]));
    return db;
}

// Level in dB of a unit sine at f (relative to the input rate) after `decimate` by `ratio`, over the settled tail.
template <typename Decimate>
double toneGainDb(double f, int ratio, int outLength, Decimate&& decimate)
{
    std::vector<float> in(static_cast<std::size_t>(outLength * ratio)), out(static_cast<std::size_t>(outLength));
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * f * static_cast<double>(n)));
    decimate(in.data(), out.data());
    double sum = 0.0;
    for (int n = outLength / 2; n < outLength; ++n)
        sum += static_cast<double>(out[static_cast<std::size_t>(n)]) * out[static_cast<std::size_t>(n)];
    return osp::measure::powerToDb(sum / (outLength / 2) / 0.5);
}

// Exact group delay of an impulse response at normalised frequency f: Re{sum n h[n] e^-jwn / H(w)}.
double groupDelay(const std::vector<float>& h, double f)
{
    std::complex<double> num, den;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const auto e = std::polar(static_cast<double>(h[n]), -2.0 * std::numbers::pi * f * static_cast<double>(n));
        num += static_cast<double>(n) * e;
        den += e;
    }
    return (num / den).real();
}

std::vector<float> render(Oversampler& os, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out(in.size(), -1.0f);
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += blockSize) {
            const int n = std::min(blockSize, total - pos);
            os.process(in.data() + pos, out.data() + pos, 0, [](float*, int) {});
            os.process(in.data() + pos, out.data() + pos, n, [](float*, int) {});
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    return out;
}

} // namespace

TEST_CASE("Oversampler: each IIR halfband stage meets ADR-0005 D3 (REQ-OS-002)", "[dsp][os]")
{
    const int stage = GENERATE(0, 1, 2);
    const auto& spec = kSpecs[stage];
    CAPTURE(stage);

    // Upsampler: impulse response at the higher rate -> FFT. Passband to 0.25 - tw, stopband from 0.25 + tw.
    constexpr int kLen = 4096;
    std::vector<float> impulse(kLen, 0.0f), h(2 * kLen);
    impulse[0] = 1.0f;
    osp::dsp::detail::runIirStage(stage, true, impulse.data(), h.data(), kLen);
    const auto db = magnitudeDb(h);
    double ripple = 0.0, stop = -400.0;
    for (std::size_t b = 0; b < db.size(); ++b) {
        const double f = static_cast<double>(b) / (2.0 * kLen);
        if (f <= 0.25 - spec.upTw)
            ripple = std::max(ripple, std::abs(db[b]));
        else if (f >= 0.25 + spec.upTw)
            stop = std::max(stop, db[b]);
    }
    std::printf("IIR stage %d up:   passband ripple %.2e dB, stopband %.1f dB (spec %.0f)\n", stage, ripple, stop,
                spec.upDb);
    CHECK(ripple <= 0.1);
    CHECK(stop <= -spec.upDb);

    // Decimator: unit sines across its stopband at the higher rate; the aliases left after decimation.
    double worst = -400.0;
    for (double f = 0.25 + spec.downTw; f < 0.4995; f += 0.0025) {
        worst = std::max(worst, toneGainDb(f, 2, kLen, [&](const float* in, float* out) {
                             osp::dsp::detail::runIirStage(stage, false, in, out, kLen);
                         }));
    }
    std::printf("IIR stage %d down: stopband %.1f dB (spec %.0f)\n", stage, worst, spec.downDb);
    CHECK(worst <= -spec.downDb);
}

TEST_CASE("Oversampler: IIR passband, images, aliases and 1 kHz delay per rate and factor (REQ-OS-001b)", "[dsp][os]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int factor = GENERATE(2, 4, 8);
    CAPTURE(fs, factor);
    constexpr int kLen = 4096;
    Oversampler os;
    os.prepare(fs, kLen, factor);

    // Identity up -> down: passband within +-0.1 dB to 20 kHz, and the 1 kHz group delay.
    std::vector<float> h(kLen, 0.0f);
    h[0] = 1.0f;
    os.process(h.data(), h.data(), kLen, [](float*, int) {});
    const auto db = magnitudeDb(h);
    double ripple = 0.0;
    for (std::size_t b = 0; static_cast<double>(b) * fs / kLen <= 20000.0; ++b)
        ripple = std::max(ripple, std::abs(db[b]));
    const double d = groupDelay(h, 1000.0 / fs);
    std::printf("IIR %gx @ %5.0f Hz: ripple to 20 kHz %.2e dB, D = %.3f, latency %d, residual %+.3f\n",
                static_cast<double>(factor), fs, ripple, d, os.getLatencySamples(), d - os.getLatencySamples());
    CHECK(ripple <= 0.1);
    CHECK(std::abs(d - os.getMeasuredDelaySamples()) < 1e-3);
    CHECK(std::abs(d - os.getLatencySamples()) <= 0.5);

    // Up only: images of the base band (f/fs in [0.6, factor/2]) at least stage 0's 90 dB down.
    os.reset();
    std::vector<float> impulse(kLen, 0.0f);
    impulse[0] = 1.0f;
    const float* up = os.upsample(impulse.data(), kLen);
    const auto upDb = magnitudeDb(std::vector<float>(up, up + kLen * factor));
    double image = -400.0;
    for (std::size_t b = 0; b < upDb.size(); ++b)
        if (static_cast<double>(b) / kLen >= 0.5 + kSpecs[0].upTw * 2.0)
            image = std::max(image, upDb[b]);
    CHECK(image <= -kSpecs[0].upDb);

    // Down only: oversampled-rate sines that fold into the base band (f/fs in [0.62, factor/2)) at least 75 dB down.
    double alias = -400.0;
    for (double f = 0.5 + kSpecs[0].downTw * 2.0; f < factor / 2.0 - 0.002; f += 0.01 * factor) {
        alias = std::max(alias, toneGainDb(f / factor, factor, kLen, [&](const float* in, float* out) {
                             os.reset();
                             for (int k = 0; k < 2; ++k) {
                                 float* buf = os.upsample(out, kLen / 2);
                                 std::copy_n(in + k * (kLen / 2) * factor, (kLen / 2) * factor, buf);
                                 os.downsample(out + k * (kLen / 2), kLen / 2);
                             }
                         }));
    }
    std::printf("IIR %gx @ %5.0f Hz: images %.1f dB, aliases %.1f dB\n", static_cast<double>(factor), fs, image, alias);
    CHECK(alias <= -kSpecs[0].downDb);
}

TEST_CASE("Oversampler: rate x block-size matrix, reset() and pre-prepare (REQ-TEST-001)", "[dsp][os]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int factor = GENERATE(1, 2, 4, 8);
    CAPTURE(fs, factor);
    const auto in = noise(4096, 7u);

    Oversampler whole;
    whole.prepare(fs, 4096, factor);
    const auto reference = render(whole, in, 4096);
    whole.reset();
    CHECK(render(whole, in, 4096) == reference); // reset() is bit-identical

    for (int block : {1, 32, 64, 127, 512, 2048}) {
        CAPTURE(block);
        Oversampler chunked;
        chunked.prepare(fs, block, factor);
        CHECK(chunked.getLatencySamples() == whole.getLatencySamples());
        CHECK(render(chunked, in, block) == reference);
        // Blocks larger than maxBlockSize are split by process().
        chunked.reset();
        CHECK(render(chunked, in, 4096) == reference);
    }

    Oversampler unprepared;
    std::vector<float> out(64, 1.0f);
    CHECK(unprepared.upsample(in.data(), 64) == nullptr);
    OSP_REQUIRE_RT_CLEAN(
        osp::rt::test::runAsAudioThread([&] { unprepared.process(in.data(), out.data(), 64, [](float*, int) {}); }));
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Oversampler: subnormal, huge and NaN input; factor changes only in prepare (REQ-OS-006)", "[dsp][os][rt]")
{
    const int factor = GENERATE(2, 4, 8);
    Oversampler os;
    os.prepare(48000.0, 64, factor);

    // Subnormal input under FTZ/DAZ gives no subnormal output; +-1e6 stays finite.
    std::vector<float> in(4096), out(4096);
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] = (n % 2 == 0 ? 1.0f : -1.0f) * (n < 2048 ? 1e-40f : 1e6f);
    out = render(os, in, 64);
    CHECK(osp::rt::test::scanOutput(out).clean());

    // A NaN poisons the IIR state until reset(); after it the render matches a fresh one bit for bit.
    const auto clean = noise(1024, 3u);
    Oversampler fresh;
    fresh.prepare(48000.0, 64, factor);
    const auto expected = render(fresh, clean, 64);
    std::vector<float> nan(64, std::nanf(""));
    render(os, nan, 64);
    os.reset();
    CHECK(render(os, clean, 64) == expected);

    // 100 quality toggles: prepare() on this (control) thread, processing on the audio thread stays RT-clean.
    for (int k = 0; k < 100; ++k) {
        os.prepare(48000.0, 64, k % 2 == 0 ? 8 : factor);
        render(os, clean, 64);
    }
}

TEST_CASE("Oversampler: silence after noise decays to exactly 0, no limit cycle", "[dsp][os][rt]")
{
    const int factor = GENERATE(2, 4, 8);
    CAPTURE(factor);
    constexpr int kBlock = 64, kRate = 48000;
    Oversampler os;
    os.prepare(kRate, kBlock, factor);
    auto in = noise(kRate, 21u);
    for (auto& v : in)
        v *= 2.0f; // full scale
    in.resize(2 * kRate, 0.0f);
    std::vector<float> out(in.size());
    bool upZero = true;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < 2 * kRate; pos += kBlock) {
            const float* up = os.upsample(in.data() + pos, kBlock);
            if (pos >= kRate + kRate / 2) // the last 0.5 s of the 1 s of silence
                upZero = upZero && std::all_of(up, up + kBlock * factor, [](float v) { return v == 0.0f; });
            os.downsample(out.data() + pos, kBlock);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(upZero);
    CHECK(std::all_of(out.end() - kRate / 2, out.end(), [](float v) { return v == 0.0f; }));
}
