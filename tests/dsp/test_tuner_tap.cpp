// Chromatic tuner, audio-thread tap (#75): decimation, lock-free hand-off to the worker, output mute. REQ-419 accuracy
// end to end through TunerTap + PitchDetector.

#include "osp/dsp/tuner/PitchDetector.h"
#include "osp/dsp/tuner/TunerTap.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

using osp::dsp::PitchDetector;

namespace {

std::vector<float> sine(double hz, double fs, int n, double amp = 0.5, double phase = 0.3)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs + phase));
    return x;
}

double centsError(double got, double want)
{
    return 1200.0 * std::log2(got / want);
}

} // namespace

namespace {

constexpr int kMaxBlock = 2048;

// Pushes `in` through the tap in blocks under the RT checker (block size 0 = an empty call before every block).
void pushAll(osp::dsp::TunerTap& tap, const std::vector<float>& in, int blockSize)
{
    const int total = static_cast<int>(in.size());
    const int step = blockSize > 0 ? blockSize : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            if (blockSize == 0)
                tap.push(in.data() + pos, 0);
            tap.push(in.data() + pos, std::min(step, total - pos));
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
}

osp::dsp::TunerReading readAndDetect(const osp::dsp::TunerTap& tap, PitchDetector& det)
{
    std::vector<float> w(static_cast<std::size_t>(det.requiredSamples()));
    REQUIRE(tap.readLatest(w.data(), static_cast<int>(w.size())));
    return det.detect(w.data(), static_cast<int>(w.size()));
}

} // namespace

TEST_CASE("TunerTap: rate x block-size matrix, tap + detector within 1 cent", "[dsp][tuner]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 32, 64, 127, 512, 2048);
    CAPTURE(fs, block);
    osp::dsp::TunerTap tap;
    tap.prepare(fs, kMaxBlock);
    REQUIRE(tap.getLatencySamples() == 0);
    PitchDetector det;
    det.prepare(tap.decimatedRate());
    for (double hz : {41.2, 82.407, 440.0, 987.77}) {
        tap.reset();
        pushAll(tap, sine(hz, fs, static_cast<int>(0.4 * fs)), block);
        const auto r = readAndDetect(tap, det);
        CAPTURE(hz);
        REQUIRE(r.valid);
        CHECK(std::abs(centsError(r.hz, hz)) <= 1.0);
    }
}

TEST_CASE("TunerTap: before prepare(), reset() and non-finite input", "[dsp][tuner]")
{
    osp::dsp::TunerTap tap;
    std::vector<float> x(256, 0.5f), dst(128);
    tap.push(x.data(), static_cast<int>(x.size())); // no-op before prepare()
    tap.applyMute(x.data(), static_cast<int>(x.size()));
    CHECK(x == std::vector<float>(256, 0.5f));
    CHECK_FALSE(tap.readLatest(dst.data(), static_cast<int>(dst.size())));

    tap.prepare(48000.0, kMaxBlock);
    PitchDetector det;
    det.prepare(tap.decimatedRate());
    const auto s = sine(196.0, 48000.0, 24000);

    // Two renders after reset() are bit-identical.
    pushAll(tap, s, 64);
    std::vector<float> a(4096), b(4096);
    REQUIRE(tap.readLatest(a.data(), 4096));
    tap.reset();
    CHECK_FALSE(tap.readLatest(b.data(), 4096));
    pushAll(tap, s, 127);
    REQUIRE(tap.readLatest(b.data(), 4096));
    CHECK(a == b);

    // NaN and Inf count as 0: the filters stay finite and the next note reads normally.
    std::vector<float> bad(4096, std::nanf(""));
    bad[100] = INFINITY;
    tap.reset();
    pushAll(tap, bad, 64);
    pushAll(tap, s, 64);
    const auto r = readAndDetect(tap, det);
    REQUIRE(r.valid);
    CHECK(std::abs(centsError(r.hz, 196.0)) <= 1.0);
}

TEST_CASE("TunerTap: output mute ramps over 10 ms and is bit-exact when off (REQ-TEST-004)", "[dsp][tuner]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    osp::dsp::TunerTap tap;
    tap.prepare(fs, kMaxBlock);
    const auto in = sine(1000.0, fs, static_cast<int>(0.05 * fs));
    auto out = in;
    tap.applyMute(out.data(), static_cast<int>(out.size()));
    CHECK(out == in); // unmuted: the samples are not touched

    const int ramp = static_cast<int>(std::lround(fs * osp::dsp::TunerTap::kMuteSeconds));
    std::vector<float> ones(static_cast<std::size_t>(ramp) * 3, 1.0f);
    tap.setMuted(true);
    const auto rt = osp::rt::test::runAsAudioThread([&] { tap.applyMute(ones.data(), static_cast<int>(ones.size())); });
    OSP_REQUIRE_RT_CLEAN(rt);
    for (int i = 1; i < ramp; ++i) {
        CHECK(ones[static_cast<std::size_t>(i)] <= ones[static_cast<std::size_t>(i) - 1]); // monotonic, no jump
        CHECK(ones[static_cast<std::size_t>(i) - 1] - ones[static_cast<std::size_t>(i)] <= 1.5f / ramp);
    }
    for (std::size_t i = static_cast<std::size_t>(ramp); i < ones.size(); ++i)
        CHECK(ones[i] == 0.0f);
    tap.setMuted(false);
    std::vector<float> back(static_cast<std::size_t>(ramp) * 2, 1.0f);
    tap.applyMute(back.data(), static_cast<int>(back.size()));
    CHECK(back.back() == 1.0f);
    out = in;
    tap.applyMute(out.data(), static_cast<int>(out.size()));
    CHECK(out == in); // idle at unity again: bit-exact
}

TEST_CASE("TunerTap: a worker reading while the audio thread pushes never gets a torn window", "[dsp][tuner]")
{
    osp::dsp::TunerTap tap;
    tap.prepare(48000.0, kMaxBlock);
    // A slow ramp stays strictly increasing through the low pass once past its start-up, so any window that mixes
    // old and new samples shows up as a step down.
    std::vector<float> rampIn(48000 * 8);
    for (std::size_t i = 0; i < rampIn.size(); ++i)
        rampIn[i] = 1e-6f * static_cast<float>(i);
    std::atomic<bool> done{false};
    int good = 0, torn = 0;
    std::thread worker([&] {
        std::vector<float> w(2048);
        while (!done.load()) {
            if (!tap.readLatest(w.data(), static_cast<int>(w.size())))
                continue;
            ++good;
            if (w.front() < 1e-3f) // still inside the filter's start-up
                continue;
            for (std::size_t i = 1; i < w.size(); ++i)
                if (w[i] < w[i - 1]) {
                    ++torn;
                    break;
                }
        }
    });
    for (std::size_t pos = 0; pos < rampIn.size(); pos += 64)
        tap.push(rampIn.data() + pos, 64);
    done = true;
    worker.join();
    WARN("worker reads accepted while pushing: " << good);
    CHECK(good > 0);
    CHECK(torn == 0);
}

TEST_CASE("TunerTap: 1000 random block / mute sequences under the RT checker", "[dsp][tuner]")
{
    osp::dsp::TunerTap tap;
    tap.prepare(48000.0, kMaxBlock);
    std::mt19937 rng(75);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1);
    std::normal_distribution<float> sig(0.0f, 0.3f);
    std::vector<float> buf(kMaxBlock);
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            const int n = len(rng);
            for (int i = 0; i < n; ++i)
                buf[static_cast<std::size_t>(i)] = sig(rng);
            if (k % 97 == 0)
                buf[0] = std::nanf("");
            tap.setMuted(coin(rng) == 1);
            tap.push(buf.data(), n);
            tap.applyMute(buf.data(), n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
}
