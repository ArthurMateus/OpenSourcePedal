// Guitar parts and tone sections, see ToneSections.h and ADR-0008 D5. Offline: allocation and exceptions are fine.

#include "osp/engine/ToneSections.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace osp::engine::tonematch {

namespace {

constexpr double kPartImbalanceDb = 12.0;  // one side this much louder ...
constexpr double kPartImbalanceShare = 0.2; // ... in this share of the windows: a guitar playing alone on that side

struct Window {
    std::size_t start = 0, end = 0; // samples
    double rmsDb = -HUGE_VAL;
    bool active = false;
    Profile profile;
};

double rmsDb(const std::vector<float>& x, std::size_t a, std::size_t b)
{
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i)
        s += static_cast<double>(x[i]) * x[i];
    return b > a && s > 0.0 ? 10.0 * std::log10(s / static_cast<double>(b - a)) : -HUGE_VAL;
}

Bands smoothed(const Bands& x) noexcept
{
    Bands out{};
    for (int k = 0; k < kBands; ++k) {
        double s = 0.0;
        int c = 0;
        for (int j = std::max(0, k - 2); j <= std::min(kBands - 1, k + 2); ++j) {
            s += x[j];
            ++c;
        }
        out[k] = s / c;
    }
    return out;
}

std::vector<Window> windows(const std::vector<float>& x, double fs, const SectionOptions& o)
{
    const auto len = static_cast<std::size_t>(o.windowSeconds * fs);
    const auto hop = std::max<std::size_t>(1, static_cast<std::size_t>(o.hopSeconds * fs));
    std::vector<Window> w;
    if (x.size() < len) {
        w.push_back({0, x.size()});
    } else {
        for (std::size_t s = 0; s + len <= x.size(); s += hop)
            w.push_back({s, s + len});
        if (w.back().end < x.size()) // the tail, as a full-length window ending at the last sample
            w.push_back({x.size() - len, x.size()});
    }
    double loudest = -HUGE_VAL;
    for (auto& v : w) {
        v.rmsDb = rmsDb(x, v.start, v.end);
        loudest = std::max(loudest, v.rmsDb);
    }
    AnalysisOptions a = o.analysis;
    a.maxSeconds = o.windowSeconds + 1.0;
    for (auto& v : w) {
        v.active = std::isfinite(v.rmsDb) && v.rmsDb >= loudest + o.silenceDb;
        if (!v.active)
            continue;
        try {
            v.profile = analyse(std::vector<float>(x.begin() + static_cast<std::ptrdiff_t>(v.start),
                                                   x.begin() + static_cast<std::ptrdiff_t>(v.end)),
                                fs, a);
        } catch (const std::invalid_argument&) { // too short or silent after the gate
            v.active = false;
        }
    }
    return w;
}

std::vector<float> source(const float* l, const float* r, std::size_t n, double fs, Source s)
{
    return pickSource(l, r, n, fs, s, nullptr);
}

// Average-linkage clustering of the active windows; returns a group id per window (-1 = inactive).
std::vector<int> cluster(const std::vector<Window>& w, const SectionOptions& o, double hopSeconds)
{
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < w.size(); ++i)
        if (w[i].active)
            idx.push_back(i);
    const std::size_t m = idx.size();
    std::vector<double> d(m * m, 0.0);
    for (std::size_t i = 0; i < m; ++i)
        for (std::size_t j = i + 1; j < m; ++j)
            d[i * m + j] = d[j * m + i] = toneDistance(w[idx[i]].profile, w[idx[j]].profile);

    std::vector<std::vector<std::size_t>> groups; // indices into idx
    for (std::size_t i = 0; i < m; ++i)
        groups.push_back({i});
    const auto link = [&](const std::vector<std::size_t>& a, const std::vector<std::size_t>& b) {
        double s = 0.0;
        for (std::size_t i : a)
            for (std::size_t j : b)
                s += d[i * m + j];
        return s / static_cast<double>(a.size() * b.size());
    };
    const auto mergeClosest = [&](bool onlySmall) {
        double best = HUGE_VAL;
        std::size_t ga = 0, gb = 0;
        for (std::size_t a = 0; a < groups.size(); ++a) {
            if (onlySmall && static_cast<double>(groups[a].size()) * hopSeconds >= o.minToneSeconds)
                continue;
            for (std::size_t b = 0; b < groups.size(); ++b) {
                if (a == b || (!onlySmall && b < a))
                    continue;
                const double l = link(groups[a], groups[b]);
                if (l < best) {
                    best = l;
                    ga = a;
                    gb = b;
                }
            }
        }
        return std::make_tuple(best, ga, gb);
    };
    while (groups.size() > 1) {
        const auto [best, a, b] = mergeClosest(false);
        if (best > o.toneThreshold)
            break;
        groups[a].insert(groups[a].end(), groups[b].begin(), groups[b].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(b));
    }
    while (groups.size() > 1) { // fills and transitions join their nearest tone
        const auto [best, a, b] = mergeClosest(true);
        if (!std::isfinite(best))
            break;
        groups[b].insert(groups[b].end(), groups[a].begin(), groups[a].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(a));
    }
    std::vector<int> id(w.size(), -1);
    for (std::size_t g = 0; g < groups.size(); ++g)
        for (std::size_t i : groups[g])
            id[idx[i]] = static_cast<int>(g);
    return id;
}

} // namespace

double toneDistance(const Profile& a, const Profile& b) noexcept
{
    const double s = residualDb(smoothed(a.levelDb), smoothed(b.levelDb));
    const double f = kFlatnessScale * flatnessDistance(a, b);
    return std::sqrt(s * s + f * f);
}

SectionResult findSections(const float* left, const float* right, std::size_t n, double fs, const SectionOptions& o)
{
    SectionResult result;
    if (right == nullptr) {
        result.parts.push_back({Source::mono, "mono"});
    } else {
        bool split = false;
        if (o.splitParts) {
            const auto l = source(left, right, n, fs, Source::left), r = source(left, right, n, fs, Source::right);
            const auto wl = windows(l, fs, o), wr = windows(r, fs, o);
            std::vector<double> dist;
            int imbalanced = 0, active = 0;
            for (std::size_t i = 0; i < wl.size(); ++i) {
                if (!wl[i].active && !wr[i].active)
                    continue;
                ++active;
                if (std::abs(wl[i].rmsDb - wr[i].rmsDb) > kPartImbalanceDb)
                    ++imbalanced;
                else if (wl[i].active && wr[i].active)
                    dist.push_back(toneDistance(wl[i].profile, wr[i].profile));
            }
            double median = 0.0;
            if (!dist.empty()) {
                std::nth_element(dist.begin(), dist.begin() + static_cast<std::ptrdiff_t>(dist.size() / 2), dist.end());
                median = dist[dist.size() / 2];
            }
            split = (active > 0 && imbalanced >= kPartImbalanceShare * active) || median > kSplitPartsDistance;
        }
        if (split) {
            result.parts.push_back({Source::left, "left"});
            result.parts.push_back({Source::right, "right"});
        } else {
            Source used = Source::mid;
            pickSource(left, right, n, fs, Source::automatic, &used);
            result.parts.push_back({used, toString(used)});
        }
    }

    for (std::size_t p = 0; p < result.parts.size(); ++p) {
        const auto x = source(left, right, n, fs, result.parts[p].source);
        const auto w = windows(x, fs, o);
        const auto id = cluster(w, o, o.hopSeconds);
        // Each window owns its central hop of time (the first and last reach the ends), so ranges tile without overlap.
        const double half = 0.5 * (o.windowSeconds - o.hopSeconds);
        std::vector<int> order; // groups in order of first appearance
        for (int g : id)
            if (g >= 0 && std::find(order.begin(), order.end(), g) == order.end())
                order.push_back(g);
        for (std::size_t t = 0; t < order.size(); ++t) {
            ToneSection sec;
            sec.part = static_cast<int>(p);
            sec.tone = static_cast<int>(t);
            for (std::size_t i = 0; i < w.size(); ++i) {
                if (id[i] != order[t])
                    continue;
                double a = static_cast<double>(w[i].start) / fs + half, b = a + o.hopSeconds;
                if (i == 0)
                    a = 0.0;
                if (i + 1 == w.size())
                    b = static_cast<double>(n) / fs;
                if (!sec.ranges.empty() && a <= sec.ranges.back().endSeconds + 1e-9)
                    sec.ranges.back().endSeconds = std::max(sec.ranges.back().endSeconds, b);
                else
                    sec.ranges.push_back({a, b});
            }
            for (const auto& r : sec.ranges)
                sec.seconds += r.endSeconds - r.startSeconds;
            result.sections.push_back(std::move(sec));
        }
    }
    if (result.sections.empty())
        throw std::invalid_argument("tonematch: no guitar loud enough to analyse");
    for (auto& sec : result.sections)
        sec.profile = analyse(sectionAudio(left, right, n, fs, result, sec), fs, o.analysis);
    return result;
}

std::vector<float> sectionAudio(const float* left, const float* right, std::size_t n, double fs,
                                const SectionResult& result, const ToneSection& section)
{
    const auto x = source(left, right, n, fs, result.parts[static_cast<std::size_t>(section.part)].source);
    std::vector<float> out;
    for (const auto& r : section.ranges) {
        const auto a = std::min(n, static_cast<std::size_t>(r.startSeconds * fs));
        const auto b = std::min(n, static_cast<std::size_t>(r.endSeconds * fs));
        out.insert(out.end(), x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
    }
    return out;
}

} // namespace osp::engine::tonematch
