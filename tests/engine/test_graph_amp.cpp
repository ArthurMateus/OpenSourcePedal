// #175 part 2: the graph builds ModelAmpChannel (ADR-0011 D1/D2). REQ-301 per channel, rate and quality, the channel
// switch between the real amps, and the sound check of the bundled DI through the default engine.

#include "../../engine/src/amp/ClassicAmpModel.h"
#include "osp/engine/Engine.h"
#include "osp/engine/Graph.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using osp::dsp::OversamplingQuality;
using osp::dsp::PartitionedConvolver;
using osp::engine::AmpParams;
using osp::engine::ClassicAmpId;
using osp::engine::ClassicAmpModel;
using osp::engine::Graph;
using osp::engine::ParamStore;
namespace m = osp::measure;

namespace {

constexpr float kDefaultGain[] = {3.0f, 5.0f, 6.0f}; // parameters.md §2.5, per channel

// A graph prepared at (rate, quality) with the cab as an identity and every other block out of the way, so the
// path from the input to the output is the amp slot and the OS-1 delay only.
struct Rig {
    ParamStore params;
    Graph graph;
    std::vector<std::unique_ptr<PartitionedConvolver>> irs;

    Rig(double rate, OversamplingQuality quality, int channel = 0) : graph(params)
    {
        for (const char* off : {"input.enabled", "gate.enabled", "comp.enabled", "output.limiter"})
            REQUIRE(params.set(off, 0.0f));
        REQUIRE(params.set("amp.channel", static_cast<float>(channel)));
        graph.prepare(rate, 512, quality);
        const float delta = 1.0f;
        for (int ch = 0; ch < Graph::kNumChannels; ++ch) {
            irs.push_back(std::make_unique<PartitionedConvolver>());
            irs.back()->prepare(&delta, 1);
            graph.setCabIr(ch, 0, irs.back().get());
        }
        graph.reset();
    }

    // Blocks of 512; `beforeBlock(pos)` runs on the audio thread before each.
    template <typename F>
    std::vector<float> render(const std::vector<float>& in, F&& beforeBlock)
    {
        std::vector<float> l(in.size()), r(in.size());
        const int total = static_cast<int>(in.size());
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += 512) {
                beforeBlock(pos);
                const auto p = static_cast<std::size_t>(pos);
                graph.process(in.data() + p, l.data() + p, r.data() + p, std::min(512, total - pos));
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(l);
        return l;
    }
    std::vector<float> render(const std::vector<float>& in)
    {
        return render(in, [](int) {});
    }
};

std::vector<double> tailOf(const std::vector<float>& x)
{
    return std::vector<double>(x.end() - static_cast<std::ptrdiff_t>(m::kCoherentFftSize), x.end());
}

double maxStep(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t i = from + 1; i < to; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    return worst;
}

} // namespace

// REQ-301 through the real amps (ADR-0011 D2/A2): L_total = round(D_os1 + D_slot) with D_slot the processing delay
// of the slot. Against the bare model (the graph differs from it by the OS-1 delay and the pad only), the 1 kHz
// phase delay the graph adds, plus the model's own processing delay, is the reported latency within +-1 sample.
TEST_CASE("Graph: REQ-301 with ModelAmpChannel on every channel, rate and quality", "[engine][graph][amp][latency]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    const int channel = GENERATE(0, 1, 2);
    CAPTURE(fs, q == OversamplingQuality::hq, channel);

    Rig rig(fs, q, channel);
    const auto tone = m::coherentTone(1000.0, fs);
    const auto warm = std::max<std::size_t>(m::kWarmupSamples, static_cast<std::size_t>(fs));
    const auto in = m::makeTone(tone, fs, -24.0, warm + m::kCoherentFftSize);
    const auto out = rig.render(in);

    AmpParams p;
    p.gain = kDefaultGain[channel];
    ClassicAmpModel bare(static_cast<ClassicAmpId>(channel));
    bare.setParameters(p);
    bare.prepare(fs, 512, q);
    std::vector<float> ref(in.size());
    for (std::size_t pos = 0; pos < in.size(); pos += 512)
        bare.process(in.data() + pos, ref.data() + pos, static_cast<int>(std::min<std::size_t>(512, in.size() - pos)));

    const double w = 2.0 * std::numbers::pi * tone.frequency / fs;
    const double added = -std::arg(m::realFft(tailOf(out))[tone.bin] / m::realFft(tailOf(ref))[tone.bin]) / w;
    const double dBare = bare.getMeasuredDelaySamples();
    std::printf("REQ-301 %5.0f Hz %s ch%d: L_total %d, D %.4f, graph adds %.4f, D_os1 + pad %.4f\n", fs,
                q == OversamplingQuality::hq ? "HQ  " : "Live", channel + 1, rig.graph.getLatencySamples(),
                rig.graph.getMeasuredDelaySamples(), added, rig.graph.getMeasuredDelaySamples() - dBare);

    // The phase measures the delay modulo one period of the tone (the HQ linear-phase delay is far longer): compare
    // the measured and the expected extra delay on the circle, then the expected one against L_total.
    const double period = fs / tone.frequency;
    const double expected = rig.graph.getMeasuredDelaySamples() - dBare;
    const double err = std::remainder(added - expected, period);
    CHECK(std::abs(err) < 0.05);
    CHECK(std::abs(rig.graph.getMeasuredDelaySamples() + err - rig.graph.getLatencySamples()) <= 1.0);
    CHECK(rig.graph.getLatencySamples() == std::lround(rig.graph.getMeasuredDelaySamples()));
}

// Hard bypass is an exact delay of L_total with the real amps built (REQ-TEST-004).
TEST_CASE("Graph: hard bypass with ModelAmpChannel is the input delayed by L_total", "[engine][graph][amp][latency]")
{
    const double fs = GENERATE(44100.0, 96000.0);
    const auto q = GENERATE(OversamplingQuality::live, OversamplingQuality::hq);
    Rig rig(fs, q);
    rig.graph.setBypassedAll(true);
    rig.graph.reset();
    std::vector<float> in(8192);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.25f * static_cast<float>(std::sin(0.07 * static_cast<double>(i * i % 997)));
    const auto out = rig.render(in);
    const auto null = m::nullTest(out, in, static_cast<std::size_t>(rig.graph.getLatencySamples()));
    CHECK(null.rmsDbfs <= -120.0);
}

// The amp.channel switch crossfades two real amps whose slot delays are padded to the same D_slot: no step beyond
// what the two settled channels produce on their own.
TEST_CASE("Graph: amp.channel crossfade between real amps has no click", "[engine][graph][amp][swap]")
{
    const double fs = 48000.0;
    const auto from = GENERATE(0, 1);
    const int to = 2;
    const auto in = m::makeTone(m::coherentTone(220.0, fs), fs, -24.0, 48000);
    const std::size_t switchAt = 24576; // a multiple of the 512 block

    Rig a(fs, OversamplingQuality::live, from), b(fs, OversamplingQuality::live, to);
    const auto outA = a.render(in), outB = b.render(in);

    Rig sw(fs, OversamplingQuality::live, from);
    const auto out = sw.render(in, [&](int pos) {
        if (static_cast<std::size_t>(pos) == switchAt)
            sw.params.set("amp.channel", static_cast<float>(to));
    });
    CHECK(sw.graph.activeChannel() == to);

    const std::size_t fadeEnd = switchAt + static_cast<std::size_t>(Graph::kSwapSeconds * fs) + 512;
    const double bound = 1.1 * std::max(maxStep(outA, 12000, in.size()), maxStep(outB, 12000, in.size())) + 1e-4;
    CHECK(maxStep(out, 12000, fadeEnd) <= bound);
}

namespace {

std::vector<float> readMono(const std::string& path, unsigned& rate)
{
    unsigned ch = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &rate, &frames, nullptr);
    REQUIRE(raw != nullptr);
    std::vector<float> x(frames);
    for (drwav_uint64 i = 0; i < frames; ++i)
        x[i] = raw[i * ch];
    drwav_free(raw, nullptr);
    return x;
}

struct Stats {
    double peak = 0.0, rmsDb = 0.0, crestDb = 0.0, rangeDb = 0.0; // rangeDb: p95 - p20 of the 50 ms window levels
};

Stats stats(const std::vector<float>& x)
{
    double peak = 0.0, sum = 0.0;
    for (const float v : x) {
        peak = std::max(peak, std::abs(static_cast<double>(v)));
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum / static_cast<double>(x.size()));
    std::vector<double> levels;
    const std::size_t win = x.size() / 200 + 1;
    for (std::size_t i = 0; i + win <= x.size(); i += win) {
        double e = 0.0;
        for (std::size_t k = i; k < i + win; ++k)
            e += static_cast<double>(x[k]) * static_cast<double>(x[k]);
        levels.push_back(10.0 * std::log10(e / static_cast<double>(win) + 1e-20));
    }
    std::sort(levels.begin(), levels.end());
    const double range = levels[levels.size() * 95 / 100] - levels[levels.size() * 20 / 100];
    return {peak, 20.0 * std::log10(rms), 20.0 * std::log10(peak / rms), range};
}

} // namespace

// Sound check (#175): the bundled DI through the default engine on each classic channel. No clipping, no NaN, and a
// clearly different gain character: the more the amp saturates, the smaller the spread of its 50 ms
// levels (it compresses).
TEST_CASE("Engine: the bundled DI renders on the three classic channels with a distinct gain character",
          "[engine][graph][amp][sound]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    REQUIRE_FALSE(di.empty());

    Stats s[3];
    for (int ch = 0; ch < 3; ++ch) {
        osp::engine::Engine engine;
        REQUIRE(engine.parameters().set("amp.channel", static_cast<float>(ch)));
        engine.prepare(static_cast<double>(rate), 512);
        engine.reset();
        std::vector<float> l = di, r(di.size());
        const int total = static_cast<int>(di.size());
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (int pos = 0; pos < total; pos += 512) {
                float* chans[] = {l.data() + pos, r.data() + pos};
                osp::engine::processChunked(engine, chans, 2, std::min(512, total - pos));
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(l);
        OSP_REQUIRE_OUTPUT_CLEAN(r);
        s[ch] = stats(l);
        std::printf("sound check ch%d: peak %.3f, RMS %.2f dBFS, crest %.2f dB, level range %.2f dB\n", ch + 1,
                    s[ch].peak, s[ch].rmsDb, s[ch].crestDb, s[ch].rangeDb);
        CHECK(s[ch].peak < 0.99); // no clipping (the output limiter ceiling is at or above this only on a clip)
        CHECK(s[ch].rmsDb > -60.0); // audible
    }
    CHECK(s[0].rangeDb > s[1].rangeDb + 1.0);
    CHECK(s[1].rangeDb > s[2].rangeDb + 0.5);
}
