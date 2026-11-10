#include "ToneMatchJob.h"

#include "AudioFiles.h"

#include "osp/dsp/conv/IrLoader.h"

#include <algorithm>
#include <chrono>
#include <exception>

namespace osp::app {

namespace fs = std::filesystem;

namespace {

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Every assets/cab/*.wav, conditioned to the DI's rate; the preset names them factory:ir/<file name without .wav>.
std::vector<osp::engine::tonematch::CabIr> loadFactoryCabs(const fs::path& assetDir, double rate, std::string& error)
{
    std::vector<osp::engine::tonematch::CabIr> cabs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(assetDir / "cab", ec)) {
        if (!e.is_regular_file() || toLower(e.path().extension().string()) != ".wav")
            continue;
        std::string why;
        const auto ir = decodeAudioFile(e.path(), 10.0, &why);
        if (!ir) {
            error = why;
            return {};
        }
        const auto name = e.path().stem().string();
        cabs.push_back(
            {name, "factory:ir/" + name, "",
             osp::dsp::IrLoader::condition(ir->left.data(), static_cast<int>(ir->left.size()), ir->sampleRate, rate)});
    }
    std::sort(cabs.begin(), cabs.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    if (cabs.empty())
        error = "no cab IRs found in " + (assetDir / "cab").string();
    return cabs;
}

} // namespace

ToneMatchJob::~ToneMatchJob()
{
    cancel();
    wait();
}

bool ToneMatchJob::start(ToneMatchSettings settings)
{
    if (running())
        return false;
    wait(); // a finished worker still has to be joined
    cancel_.store(false);
    progress_.store(0.0);
    {
        const std::lock_guard lock(mutex_);
        stage_ = "starting";
        outcome_ = {};
    }
    state_.store(State::running);
    worker_ = std::thread([this, s = std::move(settings)]() mutable { run(std::move(s)); });
    return true;
}

void ToneMatchJob::wait()
{
    if (worker_.joinable())
        worker_.join();
}

std::string ToneMatchJob::stage() const
{
    const std::lock_guard lock(mutex_);
    return stage_;
}

ToneMatchOutcome ToneMatchJob::outcome() const
{
    const std::lock_guard lock(mutex_);
    return outcome_;
}

void ToneMatchJob::finish(State state, ToneMatchOutcome outcome)
{
    {
        const std::lock_guard lock(mutex_);
        outcome_ = std::move(outcome);
    }
    state_.store(state); // last: a reader that sees the state sees the outcome
}

void ToneMatchJob::run(ToneMatchSettings s)
{
    const auto begin = std::chrono::steady_clock::now();
    ToneMatchOutcome out;
    const auto fail = [&](std::string why) {
        out.error = std::move(why);
        finish(State::failed, std::move(out));
    };
    try {
        {
            const std::lock_guard lock(mutex_);
            stage_ = "reading audio";
        }
        std::string why;
        const auto reference = decodeAudioFile(s.reference, s.maxReferenceSeconds, &why);
        if (!reference)
            return fail(why);
        const auto di = decodeAudioFile(s.di, s.maxDiSeconds, &why);
        if (!di)
            return fail(why);
        auto cabs = loadFactoryCabs(s.assetDir, di->sampleRate, why);
        if (cabs.empty())
            return fail(why);

        osp::engine::tonematch::ToneMatchRequest rq;
        rq.reference = {reference->left, reference->right, reference->sampleRate};
        rq.di = {di->left, {}, di->sampleRate};
        rq.cabs = std::move(cabs);
        rq.startSeconds = s.startSeconds;
        rq.endSeconds = s.endSeconds;
        rq.hpss = s.hpss;
        rq.presetName = s.presetName;

        const auto response = osp::engine::tonematch::runToneMatch(rq, [this](double fraction, const char* stage) {
            progress_.store(fraction);
            {
                const std::lock_guard lock(mutex_);
                stage_ = stage;
            }
            return !cancel_.load();
        });
        out.warnings = response.warnings;
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        if (response.cancelled) {
            finish(State::cancelled, std::move(out));
            return;
        }
        out.presetJson = response.presetJson;
        finish(State::finished, std::move(out));
    } catch (const std::exception& e) { // an empty or silent DI, a bad range, out of memory
        fail(e.what());
    }
}

} // namespace osp::app
