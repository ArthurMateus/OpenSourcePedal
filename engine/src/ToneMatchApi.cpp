// Tone match app API, see ToneMatchApi.h. Offline: allocation, exceptions and long loops are fine here.

#include "osp/engine/ToneMatchApi.h"

#include "osp/dsp/conv/IrLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <stdexcept>

namespace osp::engine::tonematch {

namespace {

// Share of the whole run: profiling the reference, the Matcher's candidate renders, the match, the preset.
constexpr double kProfileEnd = 0.05;
constexpr double kCandidatesEnd = 0.50;
constexpr double kMatchEnd = 0.98;

std::string makeUuid()
{
    std::random_device rd;
    std::uniform_int_distribution<int> hex(0, 15);
    std::string u = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    for (auto& c : u) {
        if (c == 'x' || c == 'y') {
            int v = hex(rd);
            if (c == 'y')
                v = 8 + (v & 3);
            c = "0123456789abcdef"[v];
        }
    }
    return u;
}

// Maps a step's local fraction into [from, to] of the whole run and keeps the whole-run fraction monotonic.
class Reporter {
public:
    explicit Reporter(const ProgressFn& fn) : fn_(fn) {}
    bool report(double from, double to, double local, const char* stage)
    {
        if (!fn_)
            return true;
        if (cancelled_) // workers still in flight must not reach the callback again
            return false;
        if (local >= 0.0)
            last_ = std::max(last_, from + (to - from) * std::min(1.0, local));
        cancelled_ = !fn_(last_, stage);
        return !cancelled_;
    }

private:
    const ProgressFn& fn_;
    double last_ = 0.0;
    bool cancelled_ = false;
};

} // namespace

ToneMatchResponse runToneMatch(const ToneMatchRequest& rq, const ProgressFn& progress)
{
    const auto t0 = std::chrono::steady_clock::now();
    const double fs = rq.di.sampleRate;
    if (!(fs >= 8000.0) || !(rq.reference.sampleRate >= 8000.0))
        throw std::invalid_argument("tonematch: sample rates must be at least 8 kHz");
    if (rq.di.left.empty() || rq.cabs.empty())
        throw std::invalid_argument("tonematch: empty DI or no cab IRs");
    const auto& rl = rq.reference.left;
    const auto& rr = rq.reference.right;
    if (rl.empty() || (!rr.empty() && rr.size() != rl.size()))
        throw std::invalid_argument("tonematch: empty reference, or channels of different lengths");

    MatchOptions opt;
    opt.analysis.hpss = rq.hpss;
    opt.searchAmps = rq.searchAmps;
    opt.threads = rq.threads;

    ToneMatchResponse out;
    Reporter rep(progress);
    try {
        if (!rep.report(0.0, kProfileEnd, 0.0, "profile"))
            throw Cancelled();
        const double rate = rq.reference.sampleRate;
        const auto from = std::min(rl.size(), static_cast<std::size_t>(std::max(0.0, rq.startSeconds) * rate));
        auto to =
            rq.endSeconds >= 0.0 ? std::min(rl.size(), static_cast<std::size_t>(rq.endSeconds * rate)) : rl.size();
        if (to <= from)
            throw std::invalid_argument("tonematch: the reference range is empty");
        to = std::min(to, from + static_cast<std::size_t>(opt.analysis.maxSeconds * rate)); // analyse() reads this much
        const std::size_t n = to - from;
        const auto mono =
            pickSource(rl.data() + from, rr.empty() ? nullptr : rr.data() + from, n, rate, rq.source, &out.sourceUsed);
        // The profile is not rate-exact between 44.1 and 48 kHz (ToneProfile.h), so profile at the DI's rate.
        const auto atDi =
            rate == fs ? mono : osp::dsp::IrLoader::resample(mono.data(), static_cast<int>(mono.size()), rate, fs);
        const Profile ref = analyse(atDi, fs, opt.analysis);
        if (!rep.report(0.0, kProfileEnd, 1.0, "profile"))
            throw Cancelled();

        opt.progress = [&](double f, const char* stage) { return rep.report(kProfileEnd, kCandidatesEnd, f, stage); };
        const Matcher matcher(rq.di.left, fs, rq.cabs, opt);
        // The Matcher keeps the hook it was built with: the match step reports into its own range.
        opt.progress = [&](double f, const char* stage) { return rep.report(kCandidatesEnd, kMatchEnd, f, stage); };
        out.match = matcher.match(ref, opt.progress);

        if (!rep.report(kMatchEnd, 1.0, 0.0, "preset"))
            throw Cancelled();
        const std::string id = rq.presetId.empty() ? makeUuid() : rq.presetId;
        out.presetJson = toPresetJson(out.match, rq.cabs, rq.presetName, id);
        rep.report(kMatchEnd, 1.0, 1.0, "done");
    } catch (const Cancelled&) {
        out = ToneMatchResponse{};
        out.cancelled = true;
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return out;
    }

    if (out.match.diTrebleDb < kDarkDiDb)
        out.warnings.push_back(
            "Your DI is dark (little treble), so clean tones will sound muffled; record a brighter DI.");
    if (!eqBandsAtLimit(out.match.setting).empty())
        out.warnings.push_back("The EQ is at its limit: the reference needs more tone shaping than the chain offers.");
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

} // namespace osp::engine::tonematch
