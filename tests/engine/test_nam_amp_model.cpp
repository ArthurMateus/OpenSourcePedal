// NamAmpModel and loadNamModel (#169, ADR-0002 D2-D4/D9, ADR-0011 P5-3): REQ-NAM-001 (loader accepts the NAM core
// example models, rejects bad files with a message), REQ-NAM-003 (native-rate null against NAM core's own process()),
// REQ-NAM-005 (an oversized block equals two maxBlock calls), REQ-NAM-010 (no-ops, reset() determinism and cost), the
// RT checker under random block sizes 0..maxBlock with resets, non-finite input fuzz, and the host-rate matrix.
// Fixtures: tests/fixtures/nam (NAM core v0.6.0 example_models, MIT, assets/LICENSES.md). The failure fixtures are
// tiny synthetic files written to a temp directory.

#include "../../engine/src/nam/NamAmpModel.h"
#include "rt_checker.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

using osp::dsp::OversamplingQuality;
using osp::engine::AmpLoadOptions;
using osp::engine::loadNamModel;
using osp::engine::ModelLoadError;
using osp::engine::NamAmpModel;
namespace fs = std::filesystem;

namespace {

constexpr double kFs = 48000.0;

fs::path fixture(const std::string& name)
{
    return fs::path(OSP_ASSET_DIR).parent_path() / "tests" / "fixtures" / "nam" / (name + ".nam");
}

// Every example model NAM core v0.6.0 ships that v1 accepts (my_model.nam is a byte copy of wavenet_a1_standard).
const std::vector<std::string> kLoadable = {"wavenet_a1_standard", "wavenet", "lstm",
                                            "slimmable_wavenet",   "A2",      "slimmable_container"};

std::unique_ptr<NamAmpModel> load(const std::string& name, const AmpLoadOptions& options = {})
{
    auto model = loadNamModel(fixture(name), options);
    auto* nam = dynamic_cast<NamAmpModel*>(model.get());
    REQUIRE(nam != nullptr);
    model.release();
    return std::unique_ptr<NamAmpModel>(nam);
}

std::vector<float> noise(std::size_t n, float amplitude, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-amplitude, amplitude);
    std::vector<float> x(n);
    for (auto& v : x)
        v = u(rng);
    return x;
}

// Renders `in` in fixed blocks under the RT checker.
std::vector<float> render(NamAmpModel& model, const std::vector<float>& in, int block)
{
    std::vector<float> out(in.size());
    const int total = static_cast<int>(in.size());
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += block)
            model.process(in.data() + pos, out.data() + pos, std::min(block, total - pos));
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(out);
    return out;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
    return d;
}

double peak(const std::vector<float>& a)
{
    double p = 0.0;
    for (float v : a)
        p = std::max(p, static_cast<double>(std::abs(v)));
    return p;
}

fs::path writeTemp(const std::string& name, const std::string& text)
{
    const fs::path p = fs::temp_directory_path() / ("osp_nam_" + name + ".nam");
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// A 1-layer LSTM, hidden size 1: 4*(in+1) + 4 + 1 + 1 cell weights, then the out x 1 head and out biases.
std::string tinyLstm(const std::string& version, int channels)
{
    const int count = 4 * (channels + 1) + 4 + 2 + 2 * channels;
    std::string w;
    for (int i = 0; i < count; ++i)
        w += (i ? ",0.1" : "0.1");
    const std::string c = std::to_string(channels);
    return R"({"version":")" + version + R"(","architecture":"LSTM","config":{"input_size":)" + c +
           R"(,"hidden_size":1,"num_layers":1,"in_channels":)" + c + R"(,"out_channels":)" + c +
           R"(},"sample_rate":48000,"weights":[)" + w + "]}";
}

std::string loadError(const fs::path& p, const AmpLoadOptions& options = {})
{
    try {
        (void)loadNamModel(p, options);
    } catch (const ModelLoadError& e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE("REQ-NAM-001: the NAM core example models load and report their metadata", "[nam][REQ-NAM-001]")
{
    for (const auto& name : kLoadable) {
        INFO(name);
        auto model = load(name);
        CHECK(model->info().trainingSampleRate == kFs); // wavenet_a1_standard has no sample_rate: 48 kHz (D4)
        CHECK(!model->info().architecture.empty());
        CHECK(model->getLatencySamples() == 0);
        CHECK(model->getMeasuredDelaySamples() == 0.0);
    }
    const auto wavenet = load("wavenet");
    CHECK(wavenet->info().architecture == "WaveNet");
    CHECK(wavenet->resetPath() == NamAmpModel::ResetPath::prewarmCache);
    CHECK(std::abs(*wavenet->info().inputLevelDbu - 18.3) < 1e-4);
    CHECK(std::abs(*wavenet->info().outputLevelDbu - 12.3) < 1e-4);
    CHECK(std::abs(*wavenet->info().loudnessDb + 20.0207) < 1e-3);
    CHECK_FALSE(wavenet->info().supportsQuality);
    const auto lstm = load("lstm");
    CHECK(lstm->resetPath() == NamAmpModel::ResetPath::lstmCells);
    CHECK(load("A2")->info().supportsQuality);
    CHECK(load("slimmable_wavenet")->info().supportsQuality);
    CHECK_FALSE(load("wavenet_a1_standard")->info().loudnessDb.has_value());
    CHECK(load("A2", AmpLoadOptions{0.25f})->info().supportsQuality); // the 0.5-width network
}

TEST_CASE("REQ-NAM-001: bad files fail with ModelLoadError and a message", "[nam][REQ-NAM-001]")
{
    REQUIRE(loadError(writeTemp("ok", tinyLstm("0.5.0", 1))).empty()); // the synthetic format itself is valid
    CHECK(loadError(writeTemp("v080", tinyLstm("0.8.0", 1))).find("unsupported model version 0.8.0") !=
          std::string::npos);
    CHECK(loadError(writeTemp("stereo", tinyLstm("0.5.0", 2))).find("only mono") != std::string::npos);
    const auto invalid = loadError(writeTemp("invalid", R"({"version":"0.5.0", "architecture": )"));
    CHECK(invalid.find("Cannot load amp model") != std::string::npos);
    CHECK(invalid.size() > 40);
    CHECK_FALSE(loadError(fixture("does_not_exist")).empty());
    // v1 limits (NamAmpModel.h): NAM core 0.6.0 has no RT-safe state restore for these, so reset() could not hold.
    CHECK(loadError(fixture("wavenet_condition_dsp")).find("condition DSP") != std::string::npos);
    CHECK(loadError(fixture("wavenet_a2_feature_test")).find("condition DSP") != std::string::npos);
    CHECK(loadError(fixture("slimmable_container"), AmpLoadOptions{0.2f}).find("LSTM") != std::string::npos);
}

TEST_CASE("REQ-NAM-003: at the model rate the output nulls against NAM core's process()", "[nam][REQ-NAM-003]")
{
    const auto in = noise(4800, 0.5f);
    for (const auto& name : kLoadable) {
        for (int block : {1, 64, 127, 512}) {
            INFO(name << " block " << block);
            auto model = load(name);
            model->prepare(kFs, block, OversamplingQuality::live);
            const auto ours = render(*model, in, block);
            const auto ref = osp::engine::renderWithNamCore(fixture(name), kFs, block, in);
            REQUIRE(peak(ref) > 1e-3);
            const double d = maxAbsDiff(ours, ref);
            if (block == 64)
                std::printf("[nam] %-20s null vs NAM core: max |diff| %.3g (%s dBFS)\n", name.c_str(), d,
                            d > 0 ? std::to_string(20 * std::log10(d)).c_str() : "-inf");
            CHECK(d < 1e-6); // -120 dBFS
        }
    }
}

TEST_CASE("REQ-NAM-005: a 2 x maxBlock call equals two maxBlock calls", "[nam][REQ-NAM-005]")
{
    const auto in = noise(64 * 2 * 20, 0.5f, 11);
    for (const auto& name : kLoadable) {
        INFO(name);
        auto a = load(name), b = load(name);
        a->prepare(kFs, 64, OversamplingQuality::live);
        b->prepare(kFs, 64, OversamplingQuality::live);
        CHECK(render(*a, in, 128) == render(*b, in, 64));
    }
}

TEST_CASE("REQ-NAM-010: no-ops, bit-identical renders after reset(), reset() cost", "[nam][REQ-NAM-010]")
{
    const auto name = GENERATE(from_range(kLoadable));
    INFO(name);
    auto model = load(name);
    std::vector<float> buf(64, 0.5f), out(64, 3.0f);
    model->process(buf.data(), out.data(), 64); // before prepare(): zero-filled
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
    model->prepare(kFs, 64, OversamplingQuality::live);
    out.assign(64, 3.0f);
    const auto rt = osp::rt::test::runAsAudioThread([&] { model->process(buf.data(), out.data(), 0); });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v == 3.0f; })); // n = 0: untouched

    const auto in = noise(4800, 0.5f, 3);
    const auto first = render(*model, in, 64); // straight after prepare()
    const auto resetRt = osp::rt::test::runAsAudioThread([&] { model->reset(); });
    OSP_REQUIRE_RT_CLEAN(resetRt);
    const auto second = render(*model, in, 64);
    model->reset();
    const auto third = render(*model, in, 64);
    CHECK(second == third);
    CHECK(first == second); // prepare() leaves exactly the state reset() restores

#ifdef NDEBUG
    // Cost, Release only: reset() < 5% of one 48 kHz / 64 block (1333 us), median of 31; one block for scale.
    std::vector<double> resets, blocks;
    for (int i = 0; i < 31; ++i) {
        model->process(in.data(), out.data(), 64);
        auto t0 = std::chrono::steady_clock::now();
        model->reset();
        auto t1 = std::chrono::steady_clock::now();
        model->process(in.data(), out.data(), 64);
        auto t2 = std::chrono::steady_clock::now();
        resets.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        blocks.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
    }
    std::nth_element(resets.begin(), resets.begin() + 15, resets.end());
    std::nth_element(blocks.begin(), blocks.begin() + 15, blocks.end());
    std::printf("[nam] %-20s reset() %.2f us (%.2f%% of a block budget), process(64) %.2f us\n", name.c_str(),
                resets[15], 100.0 * resets[15] / (64.0 / kFs * 1e6), blocks[15]);
    CHECK(resets[15] < 0.05 * 64.0 / kFs * 1e6);
#endif
}

TEST_CASE("NamAmpModel: RT checker, random blocks 0..maxBlock with resets, non-finite fuzz", "[nam][rt]")
{
    for (const std::string name : {"wavenet_a1_standard", "lstm", "A2", "slimmable_wavenet"}) {
        INFO(name);
        auto model = load(name);
        constexpr int kMax = 127;
        model->prepare(kFs, kMax, OversamplingQuality::live);
        auto in = noise(kMax * 40, 0.8f, 5);
        const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 1e30f, std::numeric_limits<float>::denorm_min()};
        for (std::size_t i = 0; i < in.size(); i += 97)
            in[i] = bad[(i / 97) % 5];
        std::vector<float> out(in.size(), 0.0f);
        std::mt19937 rng(1);
        std::uniform_int_distribution<int> size(0, kMax), coin(0, 15);
        const auto rt = osp::rt::test::runAsAudioThread([&] {
            for (std::size_t pos = 0; pos < in.size();) {
                const int n = std::min<int>(size(rng), static_cast<int>(in.size() - pos));
                model->process(in.data() + pos, out.data() + pos, n);
                if (coin(rng) == 0)
                    model->reset();
                pos += static_cast<std::size_t>(n);
            }
        });
        OSP_REQUIRE_RT_CLEAN(rt);
        OSP_REQUIRE_OUTPUT_CLEAN(out);
    }
}

TEST_CASE("NamAmpModel: host-rate x block-size matrix", "[nam]")
{
    const auto in = noise(2048 * 3, 0.5f, 9);
    for (double fs : {44100.0, 88200.0, 96000.0}) { // until #170: a clear error, never a wrong-rate render
        auto model = load("wavenet");
        try {
            model->prepare(fs, 64, OversamplingQuality::live);
            FAIL("prepare() accepted a host rate other than the model rate");
        } catch (const ModelLoadError& e) {
            CHECK(std::string(e.what()).find("#170") != std::string::npos);
        }
        CHECK_FALSE(model->isPrepared());
    }
    for (int block : {1, 32, 64, 127, 512, 2048}) {
        INFO("block " << block);
        auto model = load("wavenet");
        model->prepare(kFs, block, OversamplingQuality::live);
        const auto out = render(*model, in, block);
        CHECK(peak(out) > 1e-3);
    }
}
