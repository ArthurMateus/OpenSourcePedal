// Tone match, see ToneMatch.h and ADR-0008. Offline: allocation, exceptions and long loops are fine here.

#include "osp/engine/ToneMatch.h"

#include "osp/engine/Version.h"

#include "osp/dsp/Cab.h"
#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/Eq.h"
#include "osp/dsp/conv/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
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

struct GainFit {
    std::array<float, 9> gains{};
    double offset = 0.0; // the fitted level offset
};

// Weighted ridge least squares for the 9 band gains plus a free level offset: t ≈ Σ g_j B_j + offset.
GainFit fitGains(const EqModel& m, const Bands& t, const Bands& weights)
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
    GainFit f;
    for (std::size_t j = 0; j < 9; ++j)
        f.gains[j] = static_cast<float>(std::clamp(x[j], -12.0, 12.0));
    f.offset = x[9];
    return f;
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
// `t` excludes the cuts, so the residual is t - Σ g_j B_j - offset, around the fitted offset (#119 review).
std::array<float, 9> fitGainsRobust(const EqModel& m, const Bands& t)
{
    Bands w = defaultWeights();
    std::array<float, 9> g{};
    for (int it = 0; it < kHuberIterations; ++it) {
        const GainFit f = fitGains(m, t, w);
        g = f.gains;
        for (int k = 0; k < kBands; ++k) {
            double pred = f.offset;
            for (std::size_t j = 0; j < 9; ++j)
                pred += g[j] * m.basis[j][k];
            const double a = std::abs(t[k] - pred);
            w[k] = fitWeight(k) * (a > kHuberDb ? kHuberDb / a : 1.0);
        }
    }
    return g;
}

// Huber loss of the weighted band difference a - b around its robust location, as an RMS-equivalent dB figure
// (equal to residualDb when every band is within kHuberDb). The cut choice and the candidate ranking use it, so
// the leakage bands the robust fit ignores do not decide them either (#119 review).
double robustResidualDb(const Bands& a, const Bands& b)
{
    Bands d{}, w = defaultWeights();
    for (int k = 0; k < kBands; ++k)
        d[k] = a[k] - b[k];
    double loc = weightedMean(d);
    for (int it = 0; it < kHuberIterations; ++it) {
        double s = 0.0, ws = 0.0;
        for (int k = 0; k < kBands; ++k) {
            const double e = std::abs(d[k] - loc);
            const double wk = fitWeight(k) * (e > kHuberDb ? kHuberDb / e : 1.0);
            w[k] = wk;
            s += wk * d[k];
            ws += wk;
        }
        loc = ws > 0.0 ? s / ws : loc;
    }
    double loss = 0.0, wsum = 0.0;
    for (int k = 0; k < kBands; ++k) {
        const double e = std::abs(d[k] - loc);
        loss += fitWeight(k) * (e <= kHuberDb ? 0.5 * e * e : kHuberDb * (e - 0.5 * kHuberDb));
        wsum += fitWeight(k);
    }
    return std::sqrt(2.0 * loss / wsum);
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
            const auto g = fitGainsRobust(m, t);
            const double r = robustResidualDb(need, predictEq(m, g, lc, hc));
            if (r < best.residualDb)
                best = {g, lc, hc, r};
        }
    return best;
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

struct Matcher::State {
    std::vector<float> di;
    double fs = 0.0;
    std::vector<CabIr> cabs;
    MatchOptions o;
    EqModel eqModel;
    std::vector<Bands> cabBands;
    Setting base;
    std::vector<Setting> drives;        // drive candidates ...
    std::vector<Profile> driveProfiles; // ... and the DI through each, rendered once (ToneSections: one per tone)
    Profile untouched;                  // the DI through the default chain
};

Matcher::Matcher(std::vector<float> di, double fs, std::vector<CabIr> cabs, MatchOptions o)
    : s_(std::make_unique<State>())
{
    if (di.empty() || cabs.empty())
        throw std::invalid_argument("tonematch: empty DI or no cab IRs");
    const double diPeak = peakDb(di);
    if (!std::isfinite(diPeak))
        throw std::invalid_argument("tonematch: DI is silent");
    State& st = *s_;
    st.di = std::move(di);
    st.fs = fs;
    st.cabs = std::move(cabs);
    st.o = o;
    st.eqModel = buildEqModel(fs);
    for (const auto& c : st.cabs)
        st.cabBands.push_back(impulseBandsDb(c.ir, fs));

    // input.gainDb is -24...+24 dB (parameters.md §2.1): score the gain the preset will really apply (#119 review).
    st.base.inputGainDb = static_cast<float>(std::clamp(kDiPeakDb - diPeak, -24.0, 24.0));

    // Drive candidates: each is rendered and profiled once; cab and EQ are scored on top of it per reference.
    st.drives.push_back(st.base);
    for (DriveKind k : {DriveKind::mid, DriveKind::transparent, DriveKind::dist, DriveKind::fuzz})
        for (float knob : kDriveKnobs)
            for (float tight : kTightHz) {
                Setting d = st.base;
                d.drive = k;
                d.driveKnob = knob;
                d.tightHz = tight;
                st.drives.push_back(d);
            }
    for (const Setting& d : st.drives)
        st.driveProfiles.push_back(analyse(renderStages(st.di, fs, d, st.cabs, kDriveOnly, 256), fs, o.analysis));
    st.untouched = analyse(renderStages(st.di, fs, st.base, st.cabs, kFull, 256), fs, o.analysis);
}

Matcher::~Matcher() = default;
Matcher::Matcher(Matcher&&) noexcept = default;
Matcher& Matcher::operator=(Matcher&&) noexcept = default;

MatchResult Matcher::match(const Profile& ref) const
{
    const State& st = *s_;
    const auto& di = st.di;
    const double fs = st.fs;
    const auto& cabs = st.cabs;
    const auto& o = st.o;
    const auto& eqModel = st.eqModel;
    const auto& cabBands = st.cabBands;
    if (ref.sampleRate > 0.0 && ref.sampleRate != fs) // the profile is not rate-exact (ToneProfile.h)
        throw std::invalid_argument("tonematch: reference profiled at a different rate than the DI; resample it");

    std::vector<Candidate> all;
    for (std::size_t ci = 0; ci < st.drives.size(); ++ci) {
        const Setting& d = st.drives[ci];
        const Profile& p = st.driveProfiles[ci];
        const double flat = flatnessDistance(ref, p), spread = spreadDistance(ref, p);
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
            cand.residualDb = fit.residualDb; // robust
            cand.spreadDistance = spread;
            cand.score = o.flatnessWeight * flat * flat + o.spreadWeight * spread * spread +
                         o.spectrumWeight * fit.residualDb * fit.residualDb;
            all.push_back(cand);
        }
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    MatchResult result;
    result.reference = ref;
    result.setting = all.front().setting; // defined even if every refined score is NaN
    double bestScore = HUGE_VAL;
    int winner = 0;
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
            const auto delta = fitGainsRobust(eqModel, normalised(err));
            for (std::size_t j = 0; j < 9; ++j)
                s.eqDb[j] = std::clamp(s.eqDb[j] + delta[j], -12.0f, 12.0f);
        }
        const auto out = renderStages(di, fs, s, cabs, kFull, 256);
        const Profile got = analyse(out, fs, o.analysis);
        const double res = residualDb(ref.levelDb, got.levelDb);
        const double robust = robustResidualDb(ref.levelDb, got.levelDb);
        const double flat = flatnessDistance(ref, got), spread = spreadDistance(ref, got);
        const double score =
            o.flatnessWeight * flat * flat + o.spreadWeight * spread * spread + o.spectrumWeight * robust * robust;
        if (score < bestScore) {
            bestScore = score;
            winner = i;
            result.robustResidualDb = robust;
            s.outputDb = static_cast<float>(std::clamp(-18.0 - rmsDb(out), -60.0, 12.0));
            result.setting = s;
            result.matched = got;
            result.residualDb = res;
            result.flatnessDistance = flat;
            result.spreadDistance = spread;
        }
    }
    // The refined winner first, then the next best by predicted score (#119 review: it could be 2nd or 3rd).
    result.ranking.push_back(all[static_cast<std::size_t>(winner)]);
    for (int i = 0; i < static_cast<int>(all.size()) && result.ranking.size() < static_cast<std::size_t>(kRankingSize);
         ++i)
        if (i != winner)
            result.ranking.push_back(all[static_cast<std::size_t>(i)]);

    result.unmatchedResidualDb = residualDb(ref.levelDb, st.untouched.levelDb);
    return result;
}

MatchResult match(
    const Profile& ref, const std::vector<float>& di, double fs, const std::vector<CabIr>& cabs, const MatchOptions& o)
{
    return Matcher(di, fs, cabs, o).match(ref);
}

std::string sanitiseUtf8(const std::string& in, std::size_t maxChars)
{
    std::string out;
    std::size_t chars = 0, i = 0;
    while (i < in.size() && chars < maxChars) {
        const auto c = static_cast<unsigned char>(in[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k)
            ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            out += '?';
            ++i;
        } else if (c < 0x20) {
            out += ' '; // shortText forbids control characters
            ++i;
        } else {
            out.append(in, i, len);
            i += len;
        }
        ++chars;
    }
    return out;
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
        for (char c : sanitiseUtf8(v, 1024)) {
            if (c == '"' || c == '\\')
                o += '\\';
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
    // preset.v1: a resource that is not factory: needs its sha256 (ADR-0003 §5).
    const bool factory = cab.uri.rfind("factory:", 0) == 0;
    if (!factory && cab.sha256.size() != 64)
        throw std::invalid_argument("tonematch: a non-factory cab IR needs its sha256 for the preset");
    const std::string hash = factory ? "" : ", \"sha256\": \"" + cab.sha256 + "\"";
    const bool driveOn = s.drive != DriveKind::clean;

    std::string j = "{\n  \"author\": \"osp-tonematch\",\n  \"blocks\": [\n";
    j += "    { \"enabled\": true, \"id\": \"input\", \"params\": { \"gainDb\": " + num(s.inputGainDb) +
         " }, \"type\": \"input\" },\n";
    j += std::string("    { \"enabled\": ") + (driveOn ? "true" : "false") +
         ", \"id\": \"drive1\", \"params\": { \"drive\": " + num(s.driveKnob) + ", \"lowCutHz\": " + num(s.tightHz) +
         ", \"type\": \"" + (driveOn ? toString(s.drive) : "mid") + "\" }, \"type\": \"drive\" },\n";
    j += "    { \"channels\": [ { \"params\": { \"ir1.levelDb\": 0.00 }, \"resources\": { \"ir1\": { \"uri\": \"" +
         esc(cab.uri) + "\"" + hash +
         " } } } ], \"enabled\": true, \"id\": \"cab\", \"perChannel\": true, \"type\": \"cab\" },\n";
    j += "    { \"channels\": [ { \"params\": { " + eq +
         " } } ], \"enabled\": true, \"id\": \"eq\", \"perChannel\": true, \"type\": \"eq\" },\n";
    j += "    { \"enabled\": true, \"id\": \"output\", \"params\": { \"levelDb\": " + num(s.outputDb) +
         " }, \"type\": \"output\" }\n";
    j += "  ],\n  \"category\": \"Tone match\",\n  \"createdWith\": \"" + std::string(versionString()) +
         "\",\n  \"format\": \"osp-preset\",\n";
    j += "  \"id\": \"" + esc(uuid) + "\",\n";
    const std::string shortName = sanitiseUtf8(name.empty() ? std::string("Tone match") : name, kMaxNameChars);
    j += "  \"name\": \"" + esc(shortName) + "\",\n  \"schemaVersion\": 1,\n  \"tags\": [\"tone-match\"]\n}\n";
    return j;
}

} // namespace osp::engine::tonematch
