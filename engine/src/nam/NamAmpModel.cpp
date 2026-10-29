#include "NamAmpModel.h"

// NAM core v0.6.0 (MIT). Only this file sees NAM, Eigen and nlohmann (ADR-0002 D1, REQ-NAM-009).
#include "NAM/activations.h"
#include "NAM/container.h"
#include "NAM/convnet.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/linear.h"
#include "NAM/lstm.h"
#include "NAM/nam_file.h"
#include "NAM/sequential.h"
#include "NAM/slimmable.h"
#include "NAM/wavenet/model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace osp::engine {

namespace {

constexpr float kMaxInput = 1.0e6f; // larger or non-finite input samples are treated as 0 (as ClassicAmpModel)
constexpr double kDefaultTrainingRate = 48000.0; // a file without sample_rate (ADR-0002 D4, nam-runtime §2.3)

std::string format(const char* fmt, double a, double b)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}

// NAM core registers each architecture's parser from a static object in that architecture's .cpp. osp_nam_core is a
// static library, so the linker keeps those objects only if something references them: these references do.
using ConfigParser = std::unique_ptr<nam::ModelConfig> (*)(const nlohmann::json&, double);
const ConfigParser kArchitectures[] = {nam::wavenet::create_config,   nam::lstm::create_config,
                                       nam::container::create_config, nam::convnet::create_config,
                                       nam::linear::create_config,    nam::sequential::create_config};

// NAM core's rational tanh approximation (activations.h fast_tanh) instead of std::tanh, process-wide, as the reference
// plugin does in its constructor (NeuralAmpModelerPlugin, NeuralAmpModeler.cpp, accessed 2026-10-28). It swaps an
// entry of a static map that models read when they are built, so it runs once, before the first model is built, on
// the worker (the only thread that builds models, ADR-0004 §4). Measured on A1-Standard: see #169.
void useFastTanh()
{
    static std::once_flag once;
    std::call_once(once, [] { nam::activations::Activation::enable_fast_tanh(); });
}

// The network that runs at `quality`: a SlimmableContainer picks its first submodel with quality < max_value, else
// the last one (nam::container::ContainerModel::_get_index_for_slimmable_size at v0.6.0).
const nlohmann::json& activeNetwork(const nlohmann::json& model, double quality)
{
    if (model.value("architecture", std::string{}) != "SlimmableContainer")
        return model;
    const auto& submodels = model.at("config").at("submodels");
    for (const auto& s : submodels)
        if (quality < s.at("max_value").get<double>())
            return activeNetwork(s.at("model"), quality);
    return activeNetwork(submodels.back().at("model"), quality);
}

// How reset() restores the prewarmed state (see NamAmpModel.h); throws ModelLoadError when NAM core has no RT-safe way.
NamAmpModel::ResetPath resetPathFor(const nlohmann::json& file, double quality)
{
    const nlohmann::json& net = activeNetwork(file, quality);
    const std::string arch = net.value("architecture", std::string{});
    if (arch == "LSTM" && &net == &file)
        return NamAmpModel::ResetPath::lstmCells;
    if (arch == "WaveNet") {
        const auto& config = net.at("config");
        const auto& body = config.contains("model") ? config.at("model") : config;
        const auto cond = body.find("condition_dsp");
        if (cond == body.end() || cond->is_null())
            return NamAmpModel::ResetPath::prewarmCache;
        throw ModelLoadError(
            "a WaveNet with a condition DSP is not supported yet: NAM core 0.6.0 has no real-time-safe "
            "way to restore its state on reset");
    }
    throw ModelLoadError((&net == &file ? "architecture " : "the active network's architecture ") + arch +
                         " is not supported yet: NAM core 0.6.0 has no real-time-safe way to restore its state on "
                         "reset");
}

} // namespace

struct NamAmpModel::LstmSnapshot {
    nam::lstm::LSTM* model = nullptr;
    std::vector<nam::lstm::LSTMCell> cells; // copy-assigned back by reset(); equal Eigen sizes do not allocate
};

NamAmpModel::NamAmpModel(std::unique_ptr<nam::DSP> model, AmpModelInfo info, ResetPath path)
    : model_(std::move(model)),
      info_(std::move(info)),
      path_(path)
{}

NamAmpModel::~NamAmpModel() = default;

void NamAmpModel::prepare(double hostSampleRate, int maxBlockSize, dsp::OversamplingQuality)
{
    if (std::abs(hostSampleRate - info_.trainingSampleRate) > 1e-6)
        throw ModelLoadError(format("model sample rate %g Hz is not supported at host rate %g Hz yet (resampling "
                                    "arrives in #170)",
                                    info_.trainingSampleRate, hostSampleRate));
    const int maxBlock = std::max(maxBlockSize, 1);
    maxBlock_ = 0;
    scratch_.assign(static_cast<std::size_t>(maxBlock), 0.0f); // written once: pre-touched (RT-12)
    model_->ResetAndPrewarm(hostSampleRate, maxBlock); // sizes NAM's buffers; the first prewarm fills NAM's cache
    if (info_.supportsQuality) {
        // A slimmable WaveNet installs a staged network inside process() and releases the old one there (ADR-0002
        // D9): run that here, on the worker, so the audio thread never does.
        std::vector<float> out(1);
        float* src = scratch_.data();
        float* dst = out.data();
        model_->process(&src, &dst, 1);
    }
    if (path_ == ResetPath::prewarmCache) {
        model_->prewarm(); // now served from NAM's cache: the prepared state is exactly the state reset() restores
    } else {
        auto* lstm = dynamic_cast<nam::lstm::LSTM*>(model_.get());
        if (lstm == nullptr)
            throw ModelLoadError("internal error: LSTM reset path without an LSTM model");
        lstm_ = std::make_unique<LstmSnapshot>();
        lstm_->model = lstm;
        lstm_->cells = lstm->_layers;
    }
    maxBlock_ = maxBlock;
}

void NamAmpModel::process(const float* in, float* out, int numSamples) noexcept OSP_RT_NONBLOCKING
{
    if (numSamples <= 0)
        return;
    if (maxBlock_ == 0) {
        std::fill_n(out, numSamples, 0.0f);
        return;
    }
    // Oversized blocks are the app's job (ADR-0004 §8); chunking here keeps a mistake correct instead of overrunning.
    for (int pos = 0; pos < numSamples; pos += maxBlock_) {
        const int n = std::min(maxBlock_, numSamples - pos);
        for (int i = 0; i < n; ++i) {
            const float x = in[pos + i];
            scratch_[static_cast<std::size_t>(i)] = std::abs(x) <= kMaxInput ? x : 0.0f;
        }
        float* src = scratch_.data();
        float* dst = out + pos;
        model_->process(&src, &dst, n);
    }
}

void NamAmpModel::reset() noexcept OSP_RT_NONBLOCKING
{
    if (maxBlock_ == 0)
        return;
    if (path_ == ResetPath::prewarmCache) {
        model_->prewarm(); // cache hit after prepare(): history fill only, no model compute
    } else {
        auto& layers = lstm_->model->_layers;
        for (std::size_t i = 0; i < layers.size(); ++i)
            layers[i] = lstm_->cells[i];
    }
}

std::unique_ptr<IAmpModel> loadNamModel(const std::filesystem::path& path, const AmpLoadOptions& options)
{
    const auto name = path.filename().u8string();
    const std::string prefix = "Cannot load amp model \"" + std::string(name.begin(), name.end()) + "\": ";
    try {
        for (const ConfigParser parser : kArchitectures) // keeps the references (see kArchitectures)
            if (parser == nullptr)
                throw ModelLoadError("NAM core was built without an architecture parser");
        useFastTanh();
        const nlohmann::json file = nam::validate_nam_file(path);
        const auto version = file.at("version").get<std::string>();
        if (nam::is_version_supported(version) == nam::Supported::NO)
            throw ModelLoadError("unsupported model version " + version);
        const double quality = std::isfinite(options.quality01) ? std::clamp(options.quality01, 0.0f, 1.0f) : 1.0;

        nam::dspData data;
        auto model = nam::get_dsp(file, data);
        if (model == nullptr)
            throw ModelLoadError("NAM core returned no model");
        if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
            throw ModelLoadError(format("the model has %g input and %g output channels; only mono models (1 in, 1 "
                                        "out) are supported",
                                        static_cast<double>(model->NumInputChannels()),
                                        static_cast<double>(model->NumOutputChannels())));
        const auto resetPath = resetPathFor(file, quality);

        AmpModelInfo info;
        info.architecture = data.architecture;
        const double rate = model->GetExpectedSampleRate();
        info.trainingSampleRate = rate > 0.0 ? rate : kDefaultTrainingRate;
        if (model->HasInputLevel())
            info.inputLevelDbu = model->GetInputLevel();
        if (model->HasOutputLevel())
            info.outputLevelDbu = model->GetOutputLevel();
        if (model->HasLoudness())
            info.loudnessDb = model->GetLoudness();
        if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get())) {
            info.supportsQuality = true;
            slimmable->SetSlimmableSize(quality); // D9: a load option; prepare() installs the staged network
        }
        return std::make_unique<NamAmpModel>(std::move(model), std::move(info), resetPath);
    } catch (const std::exception& e) {
        throw ModelLoadError(prefix + e.what());
    }
}

std::vector<float>
renderWithNamCore(const std::filesystem::path& path, double sampleRate, int block, const std::vector<float>& input)
{
    useFastTanh();
    auto model = nam::get_dsp(path);
    model->Reset(sampleRate, block);
    std::vector<float> in = input, out(input.size());
    for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), in.size() - pos));
        float* src = in.data() + pos;
        float* dst = out.data() + pos;
        model->process(&src, &dst, n);
    }
    return out;
}

} // namespace osp::engine
