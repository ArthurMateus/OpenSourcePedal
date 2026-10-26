#include "osp/dsp/Cab.h"
#include "osp/dsp/conv/IrLoader.h"

#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using osp::dsp::Cab;
using osp::dsp::IrLoader;
using osp::dsp::PartitionedConvolver;

namespace {

struct Wav {
    std::vector<float> samples; // first channel
    double rate = 0.0;
};

// Reads a bundled asset (Git LFS). A pointer file means `git lfs pull` was not run: fail loudly, never skip.
Wav readAsset(const std::string& relative)
{
    const std::string path = std::string(OSP_ASSET_DIR) + "/" + relative;
    std::ifstream probe(path, std::ios::binary);
    std::string head(24, '\0');
    probe.read(head.data(), static_cast<std::streamsize>(head.size()));
    INFO(path);
    REQUIRE(head.rfind("version https://git-lfs", 0) != 0); // run `git lfs pull`

    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float* data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &frames, nullptr);
    REQUIRE(data != nullptr);
    Wav w;
    w.rate = rate;
    for (drwav_uint64 i = 0; i < frames; ++i)
        w.samples.push_back(data[i * channels]);
    drwav_free(data, nullptr);
    return w;
}

// Complex response at `hz` (direct DTFT, so the two rates compare at the same frequency).
std::complex<double> response(const std::vector<float>& h, double fs, double hz)
{
    std::complex<double> acc;
    const double w = -2.0 * std::numbers::pi * hz / fs;
    for (std::size_t n = 0; n < h.size(); ++n)
        acc += static_cast<double>(h[n]) * std::polar(1.0, w * static_cast<double>(n));
    return acc;
}

// Worst complex error of `out` vs `in` over `freqs`, in dB relative to the largest |H_in| there. The kernel adds no
// delay, so the phases compare directly; a relative measure does not blow up in the notches of a real IR.
double worstErrorDb(const std::vector<float>& in,
                    double inRate,
                    const std::vector<float>& out,
                    double outRate,
                    const std::vector<double>& freqs)
{
    double peak = 0.0, err = 0.0;
    for (double f : freqs) {
        const auto a = response(in, inRate, f), b = response(out, outRate, f);
        peak = std::max(peak, std::abs(a));
        err = std::max(err, std::abs(b - a));
    }
    return 20.0 * std::log10(err / peak);
}

std::vector<double> logFrequencies(double lo, double hi, int count)
{
    std::vector<double> f(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        f[static_cast<std::size_t>(i)] = lo * std::pow(hi / lo, static_cast<double>(i) / (count - 1));
    return f;
}

// Band-limited test IR: decaying partials below 12 kHz with a 1 ms raised-cosine onset, so nothing sits near the
// resampler cutoff and the comparison isolates the passband.
std::vector<float> bandLimitedIr(double fs, double seconds)
{
    std::vector<float> h(static_cast<std::size_t>(seconds * fs));
    const double onset = 0.001 * fs;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const double t = static_cast<double>(n) / fs;
        double v = 0.0;
        for (double f : {150.0, 900.0, 2500.0, 6000.0, 11000.0})
            v += std::sin(2.0 * std::numbers::pi * f * t) * std::exp(-t * f / 40.0);
        const double ramp = static_cast<double>(n) < onset ? 0.5 * (1.0 - std::cos(std::numbers::pi * n / onset)) : 1.0;
        h[n] = static_cast<float>(0.2 * v * ramp);
    }
    return h;
}

} // namespace

TEST_CASE("IrLoader: same rate is an exact copy; non-finite samples become 0", "[dsp][cab][ir]")
{
    std::vector<float> h{0.5f, -0.25f, std::nanf(""), 0.125f, INFINITY};
    const auto out = IrLoader::condition(h.data(), static_cast<int>(h.size()), 48000.0, 48000.0);
    REQUIRE(out == std::vector<float>{0.5f, -0.25f, 0.0f, 0.125f, 0.0f});
    REQUIRE(IrLoader::condition(nullptr, 0, 48000.0, 48000.0).empty());
}

TEST_CASE("IrLoader: resampling preserves the frequency response", "[dsp][cab][ir]")
{
    const double from = GENERATE(44100.0, 48000.0, 96000.0);
    const double to = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(from, to);
    const auto h = bandLimitedIr(from, 0.1);
    const auto out = IrLoader::resample(h.data(), static_cast<int>(h.size()), from, to);
    REQUIRE(out.size() == static_cast<std::size_t>(std::ceil(h.size() * to / from)));
    const double errDb = worstErrorDb(h, from, out, to, logFrequencies(50.0, 16000.0, 60));
    CAPTURE(errDb);
    REQUIRE(errDb < -90.0); // measured worst -105.4 dB over the 12 rate pairs
}

TEST_CASE("IrLoader: the 96 kHz 1x12 asset resamples to 48 kHz with its response intact", "[dsp][cab][ir][assets]")
{
    const auto w = readAsset("cab/112-a.wav");
    REQUIRE(w.rate == 96000.0);
    const auto out = IrLoader::condition(w.samples.data(), static_cast<int>(w.samples.size()), w.rate, 48000.0);
    REQUIRE(out.size() == w.samples.size() / 2);
    // A real IR has its transient at sample 13 of 96 kHz, so the truncated pre-ringing costs a little near the top.
    const double errDb = worstErrorDb(w.samples, w.rate, out, 48000.0, logFrequencies(50.0, 16000.0, 120));
    CAPTURE(errDb);
    REQUIRE(errDb < -40.0); // measured -50.0 dB
}

TEST_CASE("IrLoader: IRs longer than 1 s are capped with a raised-cosine fade", "[dsp][cab][ir][assets]")
{
    const auto w = readAsset("cab/412-a.wav"); // 1.196 s at 48 kHz
    const double fs = GENERATE(44100.0, 48000.0);
    CAPTURE(fs);
    const auto out = IrLoader::condition(w.samples.data(), static_cast<int>(w.samples.size()), w.rate, fs);
    const auto cap = static_cast<std::size_t>(fs);
    REQUIRE(out.size() == cap);
    const auto fade = static_cast<std::size_t>(std::lround(IrLoader::kCapFadeSeconds * fs));
    if (fs == w.rate) // before the fade the taps are untouched
        REQUIRE(std::equal(out.begin(), out.end() - static_cast<long>(fade), w.samples.begin()));
    const float tailPeak = *std::max_element(w.samples.begin(), w.samples.end(),
                                             [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(out.back()) <= 1e-3f * std::abs(tailPeak)); // faded to (almost) nothing at the cut
}

TEST_CASE("IrLoader + Cab: every bundled IR loads at every rate and renders the DI clip cleanly",
          "[dsp][cab][ir][assets]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE(fs);
    const auto di = readAsset("di/strum-arp-01.wav");
    std::vector<float> x(static_cast<std::size_t>(fs / 2)); // 0.5 s of the clip, as-is (the rate is not what's tested)
    std::copy_n(di.samples.begin(), std::min(x.size(), di.samples.size()), x.begin());

    std::vector<std::unique_ptr<PartitionedConvolver>> irs;
    for (const char* name : {"112-a", "412-a", "412-b", "412-c", "412-d", "412-e", "412-f"}) {
        const auto w = readAsset(std::string("cab/") + name + ".wav");
        const auto taps = IrLoader::condition(w.samples.data(), static_cast<int>(w.samples.size()), w.rate, fs);
        CAPTURE(name);
        REQUIRE(taps.size() <= static_cast<std::size_t>(std::lround(IrLoader::kMaxIrSeconds * fs)));
        irs.push_back(std::make_unique<PartitionedConvolver>());
        irs.back()->prepare(taps.data(), static_cast<int>(taps.size()));
    }

    Cab cab;
    cab.setIr(0, irs[0].get());
    cab.setIr(1, irs[1].get());
    cab.setSlotEnabled(1, true);
    cab.prepare(fs, 512);
    std::vector<float> l(x.size()), r(x.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        std::size_t next = 2;
        for (std::size_t pos = 0; pos < x.size(); pos += 512) {
            if (pos % 8192 == 0 && next < irs.size()) // swap through the whole set while playing
                cab.setIr(static_cast<int>(next % 2), irs[next++].get());
            const int n = static_cast<int>(std::min<std::size_t>(512, x.size() - pos));
            cab.process(x.data() + pos, l.data() + pos, r.data() + pos, n);
            while (cab.popRetired() != nullptr) {
            }
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
    const float peak =
        *std::max_element(l.begin(), l.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(peak) > 1e-3f); // the IRs actually produced output
}
