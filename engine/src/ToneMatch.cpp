// Tone match, see ToneMatch.h and ADR-0008. Offline: allocation, exceptions and long loops are fine here.

#include "osp/engine/ToneMatch.h"

#include "osp/dsp/Cab.h"
#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/Eq.h"
#include "osp/dsp/conv/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace osp::engine::tonematch {

namespace {

constexpr int kImpulseLength = 8192; // EQ impulse length for its band response
constexpr double kRidge = 0.05; // per dB² of band gain: keeps the graphic EQ from chasing narrow detail
constexpr double kHuberDb = 3.0;
constexpr int kHuberIterations = 4;
constexpr std::array<float, 6> kDriveKnobs{0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f};
constexpr std::array<float, 2> kTightHz{20.0f, 100.0f};
constexpr std::array<float, 7> kLowCutHz{20.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f, 200.0f};
constexpr std::array<float, 6> kHighCutHz{20000.0f, 12000.0f, 9000.0f, 7000.0f, 5500.0f, 4500.0f};
constexpr int kRefineCandidates = 3;
constexpr int kRankingSize = 5;

// Solves the dense system A x = b in place (Gaussian elimination with partial pivoting). A is n x n, row-major.
std::vector<double> solve(std::vector<double> A, std::vector<double> b)
{
    const auto n = b.size();
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t p = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::abs(A[r * n + c]) > std::abs(A[p * n + c]))
                p = r;
        for (std::size_t k = 0; k < n; ++k)
            std::swap(A[c * n + k], A[p * n + k]);
        std::swap(b[c], b[p]);
        const double d = A[c * n + c];
        if (std::abs(d) < 1e-12)
            continue;
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / d;
            for (std::size_t k = c; k < n; ++k)
                A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    std::vector<double> x(n, 0.0);
    for (std::size_t c = n; c-- > 0;) {
        double s = b[c];
        for (std::size_t k = c + 1; k < n; ++k)
            s -= A[c * n + k] * x[k];
        x[c] = std::abs(A[c * n + c]) < 1e-12 ? 0.0 : s / A[c * n + c];
    }
    return x;
}

// The graphic EQ as a linear model in dB: response(g, cuts) ≈ cutLow + cutHigh + Σ g_j · basis_j. The bell's
// dB shape is not exactly proportional to its gain, so match() refines the fit against the real block.
struct EqModel {
    std::array<Bands, 9> basis{};                 // dB per dB of band gain
    std::array<Bands, kLowCutHz.size()> lowCut{}; // dB
    std::array<Bands, kHighCutHz.size()> highCut{};
};

std::vector<float> eqImpulse(double fs, const std::array<float, 9>& g, float lowCut, float highCut)
{
    osp::dsp::Eq eq;
    eq.prepare(fs, kImpulseLength);
    for (int j = 0; j < 9; ++j)
        eq.setGraphic9Db(j, g[static_cast<std::size_t>(j)]);
    eq.setLowCutHz(lowCut);
    eq.setHighCutHz(highCut);
    eq.reset();
    std::vector<float> x(static_cast<std::size_t>(kImpulseLength), 0.0f);
    x[0] = 1.0f;
    float* ch[1] = {x.data()};
    eq.process(ch, 1, static_cast<int>(x.size()));
    return x;
}

EqModel buildEqModel(double fs)
{
    EqModel m;
    const std::array<float, 9> flat{};
    for (int j = 0; j < 9; ++j) {
        auto g = flat;
        g[static_cast<std::size_t>(j)] = 6.0f;
        const Bands r = impulseBandsDb(eqImpulse(fs, g, 20.0f, 20000.0f), fs);
        for (int k = 0; k < kBands; ++k)
            m.basis[static_cast<std::size_t>(j)][k] = r[k] / 6.0;
    }
    for (std::size_t i = 0; i < kLowCutHz.size(); ++i)
        m.lowCut[i] = impulseBandsDb(eqImpulse(fs, flat, kLowCutHz[i], 20000.0f), fs);
    for (std::size_t i = 0; i < kHighCutHz.size(); ++i)
        m.highCut[i] = impulseBandsDb(eqImpulse(fs, flat, 20.0f, kHighCutHz[i]), fs);
    return m;
}

// Weighted ridge least squares for the 9 band gains plus a free level offset: t ≈ Σ g_j B_j + offset.
std::array<float, 9> fitGains(const EqModel& m, const Bands& t, const Bands& weights)
{
    constexpr std::size_t n = 10;
    std::vector<double> A(n * n, 0.0), b(n, 0.0);
    for (int k = 0; k < kBands; ++k) {
        const double w = weights[k];
        if (w == 0.0)
            continue;
        std::array<double, n> row{};
        for (std::size_t j = 0; j < 9; ++j)
            row[j] = m.basis[j][k];
        row[9] = 1.0;
        for (std::size_t r = 0; r < n; ++r) {
            b[r] += w * row[r] * t[k];
            for (std::size_t c = 0; c < n; ++c)
                A[r * n + c] += w * row[r] * row[c];
        }
    }
    double wSum = 0.0;
    for (int k = 0; k < kBands; ++k)
        wSum += weights[k];
    for (std::size_t j = 0; j < 9; ++j)
        A[j * n + j] += kRidge * wSum;
    const auto x = solve(A, b);
    std::array<float, 9> g{};
    for (std::size_t j = 0; j < 9; ++j)
        g[j] = static_cast<float>(std::clamp(x[j], -12.0, 12.0));
    return g;
}

Bands predictEq(const EqModel& m, const std::array<float, 9>& g, std::size_t lc, std::size_t hc)
{
    Bands r{};
    for (int k = 0; k < kBands; ++k) {
        double v = m.lowCut[lc][k] + m.highCut[hc][k];
        for (std::size_t j = 0; j < 9; ++j)
            v += g[j] * m.basis[j][k];
        r[k] = v;
    }
    return r;
}

Bands defaultWeights()
{
    Bands w{};
    for (int k = 0; k < kBands; ++k)
        w[k] = fitWeight(k);
    return w;
}

// Huber IRLS (P. J. Huber, "Robust Statistics", 1981): a band the EQ cannot reach (bass or vocal leakage in a full
// mix) is down-weighted by kHuberDb / |residual| instead of dragging every other band to the +-12 dB limits.
std::array<float, 9> fitGainsRobust(const EqModel& m, const Bands& t, std::size_t lc, std::size_t hc)
{
    Bands w = defaultWeights();
    std::array<float, 9> g{};
    for (int it = 0; it < kHuberIterations; ++it) {
        g = fitGains(m, t, w);
        const Bands pred = predictEq(m, g, lc, hc);
        Bands r{};
        for (int k = 0; k < kBands; ++k)
            r[k] = t[k] + m.lowCut[lc][k] + m.highCut[hc][k] - pred[k];
        const double off = weightedMean(r);
        for (int k = 0; k < kBands; ++k) {
            const double a = std::abs(r[k] - off);
            w[k] = fitWeight(k) * (a > kHuberDb ? kHuberDb / a : 1.0);
        }
    }
    return g;
}

struct EqFit {
    std::array<float, 9> gains{};
    std::size_t lowCut = 0, highCut = 0;
    double residualDb = 0.0;
};

// `need` = reference - (drive + cab), the response the EQ has to add. Tries every cut pair.
EqFit fitEq(const EqModel& m, const Bands& need)
{
    EqFit best;
    best.residualDb = HUGE_VAL;
    for (std::size_t lc = 0; lc < kLowCutHz.size(); ++lc)
        for (std::size_t hc = 0; hc < kHighCutHz.size(); ++hc) {
            Bands t{};
            for (int k = 0; k < kBands; ++k)
                t[k] = need[k] - m.lowCut[lc][k] - m.highCut[hc][k];
            const auto g = fitGainsRobust(m, t, lc, hc);
            const double r = residualDb(need, predictEq(m, g, lc, hc));
            if (r < best.residualDb)
                best = {g, lc, hc, r};
        }
    return best;
}

std::size_t indexOf(const auto& table, float v)
{
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i] == v)
            return i;
    return 0;
}

double peakDb(const std::vector<float>& x)
{
    float p = 0.0f;
    for (float v : x)
        p = std::max(p, std::abs(v));
    return p > 0.0f ? 20.0 * std::log10(p) : -HUGE_VAL;
}

double rmsDb(const std::vector<float>& x)
{
    double s = 0.0;
    for (float v : x)
        s += static_cast<double>(v) * v;
    return x.empty() || s == 0.0 ? -HUGE_VAL : 10.0 * std::log10(s / static_cast<double>(x.size()));
}

osp::dsp::DriveType driveType(DriveKind k)
{
    switch (k) {
    case DriveKind::transparent:
        return osp::dsp::DriveType::transparent;
    case DriveKind::dist:
        return osp::dsp::DriveType::dist;
    case DriveKind::fuzz:
        return osp::dsp::DriveType::fuzz;
    default:
        return osp::dsp::DriveType::mid;
    }
}

enum Stages { kDriveOnly = 1, kFull = 2 };

std::vector<float> renderStages(
    const std::vector<float>& di, double fs, const Setting& s, const std::vector<CabIr>& cabs, int stages, int block)
{
    const auto n = di.size();
    std::vector<float> x(n);
    const float in = std::pow(10.0f, s.inputGainDb / 20.0f);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = di[i] * in;

    if (s.drive != DriveKind::clean) {
        osp::dsp::DriveRegion drive;
        drive.prepare(fs, block);
        auto& d = drive.slot(0);
        d.setType(driveType(s.drive));
        d.setDrive(s.driveKnob);
        d.setLowCutHz(s.tightHz);
        d.setEnabled(true);
        drive.reset();
        for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(block)) {
            const int len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), n - pos));
            drive.process(x.data() + pos, x.data() + pos, len);
        }
    }
    if (stages == kDriveOnly)
        return x;

    if (!cabs.empty()) {
        const auto& ir = cabs[static_cast<std::size_t>(std::clamp(s.cab, 0, static_cast<int>(cabs.size()) - 1))].ir;
        osp::dsp::PartitionedConvolver conv;
        conv.prepare(ir.data(), static_cast<int>(ir.size()));
        osp::dsp::Cab cab;
        cab.setIr(0, &conv);
        cab.prepare(fs, block);
        cab.reset();
        std::vector<float> right(static_cast<std::size_t>(block));
        for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(block)) {
            const int len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), n - pos));
            cab.process(x.data() + pos, x.data() + pos, right.data(), len); // centre pan: L = R = mono
        }
        cab.retireAll();
    }

    osp::dsp::Eq eq;
    eq.prepare(fs, block);
    for (int j = 0; j < 9; ++j)
        eq.setGraphic9Db(j, s.eqDb[static_cast<std::size_t>(j)]);
    eq.setLowCutHz(s.eqLowCutHz);
    eq.setHighCutHz(s.eqHighCutHz);
    eq.reset();
    const float out = std::pow(10.0f, s.outputDb / 20.0f);
    for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(block)) {
        const int len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), n - pos));
        float* ch[1] = {x.data() + pos};
        eq.process(ch, 1, len);
        for (int i = 0; i < len; ++i)
            x[pos + static_cast<std::size_t>(i)] *= out;
    }
    return x;
}

} // namespace

const char* toString(DriveKind k) noexcept
{
    switch (k) {
    case DriveKind::mid:
        return "mid";
    case DriveKind::transparent:
        return "transparent";
    case DriveKind::dist:
        return "dist";
    case DriveKind::fuzz:
        return "fuzz";
    default:
        return "clean";
    }
}

std::vector<float>
render(const std::vector<float>& di, double fs, const Setting& s, const std::vector<CabIr>& cabs, int blockSize)
{
    return renderStages(di, fs, s, cabs, kFull, blockSize);
}

MatchResult match(
    const Profile& ref, const std::vector<float>& di, double fs, const std::vector<CabIr>& cabs, const MatchOptions& o)
{
    if (di.empty() || cabs.empty())
        throw std::invalid_argument("tonematch: empty DI or no cab IRs");
    const double diPeak = peakDb(di);
    if (!std::isfinite(diPeak))
        throw std::invalid_argument("tonematch: DI is silent");

    const EqModel eqModel = buildEqModel(fs);
    std::vector<Bands> cabBands;
    for (const auto& c : cabs)
        cabBands.push_back(impulseBandsDb(c.ir, fs));

    Setting base;
    base.inputGainDb = static_cast<float>(kDiPeakDb - diPeak);

    // Drive candidates: each is rendered and profiled once; cab and EQ are scored on top of it.
    std::vector<Setting> drives{base};
    for (DriveKind k : {DriveKind::mid, DriveKind::transparent, DriveKind::dist, DriveKind::fuzz})
        for (float knob : kDriveKnobs)
            for (float tight : kTightHz) {
                Setting s = base;
                s.drive = k;
                s.driveKnob = knob;
                s.tightHz = tight;
                drives.push_back(s);
            }

    std::vector<Candidate> all;
    for (const Setting& d : drives) {
        const Profile p = analyse(renderStages(di, fs, d, cabs, kDriveOnly, 256), fs, o.analysis);
        const double flat = flatnessDistance(ref, p);
        for (std::size_t c = 0; c < cabs.size(); ++c) {
            Bands need{};
            for (int k = 0; k < kBands; ++k)
                need[k] = ref.levelDb[k] - p.levelDb[k] - cabBands[c][k];
            const EqFit fit = fitEq(eqModel, normalised(need));
            Candidate cand;
            cand.setting = d;
            cand.setting.cab = static_cast<int>(c);
            cand.setting.eqDb = fit.gains;
            cand.setting.eqLowCutHz = kLowCutHz[fit.lowCut];
            cand.setting.eqHighCutHz = kHighCutHz[fit.highCut];
            cand.flatnessDistance = flat;
            cand.residualDb = fit.residualDb;
            cand.score = o.flatnessWeight * flat * flat + o.spectrumWeight * fit.residualDb * fit.residualDb;
            all.push_back(cand);
        }
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    MatchResult result;
    result.reference = ref;
    double bestScore = HUGE_VAL;
    for (int i = 0; i < std::min<int>(kRefineCandidates, static_cast<int>(all.size())); ++i) {
        Setting s = all[static_cast<std::size_t>(i)].setting;
        // Refine the gains against the real Eq's response: the additive model's bell shapes are approximate.
        const Profile pre = analyse(renderStages(
                                        di, fs,
                                        [&] {
                                            Setting flat = s;
                                            flat.eqDb = {};
                                            flat.eqLowCutHz = 20.0f;
                                            flat.eqHighCutHz = 20000.0f;
                                            return flat;
                                        }(),
                                        cabs, kFull, 256),
                                    fs, o.analysis);
        Bands need{};
        for (int k = 0; k < kBands; ++k)
            need[k] = ref.levelDb[k] - pre.levelDb[k];
        need = normalised(need);
        for (int it = 0; it < 3; ++it) {
            const Bands actual = impulseBandsDb(eqImpulse(fs, s.eqDb, s.eqLowCutHz, s.eqHighCutHz), fs);
            Bands err{};
            for (int k = 0; k < kBands; ++k)
                err[k] = need[k] - actual[k];
            const auto delta = fitGainsRobust(eqModel, normalised(err), indexOf(kLowCutHz, s.eqLowCutHz),
                                              indexOf(kHighCutHz, s.eqHighCutHz));
            for (std::size_t j = 0; j < 9; ++j)
                s.eqDb[j] = std::clamp(s.eqDb[j] + delta[j], -12.0f, 12.0f);
        }
        const auto out = renderStages(di, fs, s, cabs, kFull, 256);
        const Profile got = analyse(out, fs, o.analysis);
        const double res = residualDb(ref.levelDb, got.levelDb);
        const double flat = flatnessDistance(ref, got);
        const double score = o.flatnessWeight * flat * flat + o.spectrumWeight * res * res;
        if (score < bestScore) {
            bestScore = score;
            s.outputDb = static_cast<float>(std::clamp(-18.0 - rmsDb(out), -60.0, 12.0));
            result.setting = s;
            result.matched = got;
            result.residualDb = res;
            result.flatnessDistance = flat;
        }
    }
    for (int i = 0; i < std::min<int>(kRankingSize, static_cast<int>(all.size())); ++i)
        result.ranking.push_back(all[static_cast<std::size_t>(i)]);

    const Profile untouched = analyse(renderStages(di, fs, base, cabs, kFull, 256), fs, o.analysis);
    result.unmatchedResidualDb = residualDb(ref.levelDb, untouched.levelDb);
    return result;
}

std::string
toPresetJson(const MatchResult& r, const std::vector<CabIr>& cabs, const std::string& name, const std::string& uuid)
{
    const Setting& s = r.setting;
    const auto num = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", v);
        return std::string(b);
    };
    const auto esc = [](const std::string& v) {
        std::string o;
        for (char c : v) {
            if (c == '"' || c == '\\')
                o += '\\';
            if (static_cast<unsigned char>(c) >= 0x20)
                o += c;
        }
        return o;
    };
    static constexpr std::array<const char*, 9> kEqKeys{"g63Db", "g125Db", "g250Db", "g500Db", "g1kDb",
                                                        "g2kDb", "g4kDb",  "g8kDb",  "g16kDb"};
    std::string eq;
    for (std::size_t j = 0; j < 9; ++j)
        eq += std::string("\"") + kEqKeys[j] + "\": " + num(s.eqDb[j]) + ", ";
    eq += "\"highCutHz\": " + num(s.eqHighCutHz) + ", \"lowCutHz\": " + num(s.eqLowCutHz) + ", \"mode\": \"graphic9\"";
    const auto& cab = cabs[static_cast<std::size_t>(std::clamp(s.cab, 0, static_cast<int>(cabs.size()) - 1))];
    const bool driveOn = s.drive != DriveKind::clean;

    std::string j =
        "{\n  \"$schema\": \"../schema/preset.v1.schema.json\",\n  \"author\": \"osp-tonematch\",\n  \"blocks\": [\n";
    j += "    { \"enabled\": true, \"id\": \"input\", \"params\": { \"gainDb\": " + num(s.inputGainDb) +
         " }, \"type\": \"input\" },\n";
    j += std::string("    { \"enabled\": ") + (driveOn ? "true" : "false") +
         ", \"id\": \"drive1\", \"params\": { \"drive\": " + num(s.driveKnob) + ", \"lowCutHz\": " + num(s.tightHz) +
         ", \"type\": \"" + (driveOn ? toString(s.drive) : "mid") + "\" }, \"type\": \"drive\" },\n";
    j += "    { \"channels\": [ { \"params\": { \"ir1.levelDb\": 0.00 }, \"resources\": { \"ir1\": { \"uri\": \"" +
         esc(cab.uri) + "\" } } } ], \"enabled\": true, \"id\": \"cab\", \"perChannel\": true, \"type\": \"cab\" },\n";
    j += "    { \"channels\": [ { \"params\": { " + eq +
         " } } ], \"enabled\": true, \"id\": \"eq\", \"perChannel\": true, \"type\": \"eq\" },\n";
    j += "    { \"enabled\": true, \"id\": \"output\", \"params\": { \"levelDb\": " + num(s.outputDb) +
         " }, \"type\": \"output\" }\n";
    j += "  ],\n  \"category\": \"Tone match\",\n  \"createdWith\": \"0.1.0\",\n  \"format\": \"osp-preset\",\n";
    j += "  \"id\": \"" + esc(uuid) + "\",\n";
    j += "  \"name\": \"" + esc(name) + "\",\n  \"schemaVersion\": 1,\n  \"tags\": [\"tone-match\"]\n}\n";
    return j;
}

} // namespace osp::engine::tonematch
