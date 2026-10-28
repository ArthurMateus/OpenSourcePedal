#include "osp/engine/Params.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace osp::engine {
namespace {

using Names = std::span<const std::string_view>;

constexpr std::array<std::string_view, 2> kDetector{"peak", "rms"};
constexpr std::array<std::string_view, 3> kCompMode{"opto", "vca", "dual"};
constexpr std::array<std::string_view, 4> kDriveType{"mid", "transparent", "dist", "fuzz"};
constexpr std::array<std::string_view, 3> kChannel{"1", "2", "3"};
constexpr std::array<std::string_view, 3> kNamLevel{"raw", "normalized", "calibrated"};
constexpr std::array<std::string_view, 3> kEqMode{"graphic9", "graphic5", "parametric"};
constexpr std::array<std::string_view, 2> kEdgeShape{"peak", "shelf"};
constexpr std::array<std::string_view, 6> kModType{"chorus80s", "chorusWide", "flanger", "phaser", "vibe", "tremolo"};
constexpr std::array<std::string_view, 16> kModSync{"off",  "1/1",  "1/2",   "1/4",  "1/8",  "1/16", "1/1D", "1/2D",
                                                    "1/4D", "1/8D", "1/16D", "1/1T", "1/2T", "1/4T", "1/8T", "1/16T"};
constexpr std::array<std::string_view, 2> kModPosition{"post", "pre"};
constexpr std::array<std::string_view, 3> kDelayMode{"digital", "analog", "tape"};
// dsp::SyncDivision order: off, then triplet, straight, dotted per note value from 1/64 to 1/1.
constexpr std::array<std::string_view, 22> kDelaySync{
    "off", "1/64T", "1/64", "1/64D", "1/32T", "1/32", "1/32D", "1/16T", "1/16", "1/16D", "1/8T",
    "1/8", "1/8D",  "1/4T", "1/4",   "1/4D",  "1/2T", "1/2",   "1/2D",  "1/1T", "1/1",   "1/1D"};
constexpr std::array<std::string_view, 3> kDelayStereo{"mono", "pingpong", "wide"};
constexpr std::array<std::string_view, 5> kReverbMode{"room", "hall", "plate", "spring", "shimmer"};
constexpr std::array<std::string_view, 3> kShimmer{"octUp", "fifthUp", "octDown"};
constexpr std::array<std::string_view, 9> kGraphic9{"g63Db", "g125Db", "g250Db", "g500Db", "g1kDb",
                                                    "g2kDb", "g4kDb",  "g8kDb",  "g16kDb"};
constexpr std::array<std::string_view, 5> kGraphic5{"m80Db", "m240Db", "m750Db", "m2200Db", "m6600Db"};

class Builder {
public:
    std::vector<ParamInfo> table;
    std::string prefix; // "<blockId>." or "<blockId>.ch<N>."

    void
    lin(std::string_view key, float min, float max, float def, std::string_view unit, Visibility vis = Visibility::host)
    {
        add(key, min, max, def, Skew::lin, ParamKind::continuous, vis, unit, {});
    }
    void log(std::string_view key, float min, float max, float def, std::string_view unit)
    {
        add(key, min, max, def, Skew::log, ParamKind::continuous, Visibility::host, unit, {});
    }
    void boolean(std::string_view key, bool def, Visibility vis = Visibility::host)
    {
        add(key, 0.0f, 1.0f, def ? 1.0f : 0.0f, Skew::step, ParamKind::boolean, vis, {}, {});
    }
    void choice(std::string_view key, Names names, int def, Visibility vis = Visibility::host)
    {
        add(key, 0.0f, static_cast<float>(names.size() - 1), static_cast<float>(def), Skew::step, ParamKind::choice,
            vis, {}, names);
    }
    void integer(std::string_view key, int min, int max, int def, std::string_view unit, Visibility vis)
    {
        add(key, static_cast<float>(min), static_cast<float>(max), static_cast<float>(def), Skew::step,
            ParamKind::integer, vis, unit, {});
    }

private:
    void add(std::string_view key,
             float min,
             float max,
             float def,
             Skew skew,
             ParamKind kind,
             Visibility vis,
             std::string_view unit,
             Names names)
    {
        table.push_back({prefix + std::string(key), min, max, def, skew, kind, vis, unit, names});
    }
};

std::string channelPrefix(std::string_view block, int ch)
{
    return std::string(block) + ".ch" + std::to_string(ch) + ".";
}

// parameters.md §2–§3, in document order.
std::vector<ParamInfo> buildTable()
{
    constexpr auto P = Visibility::preset;
    Builder b;
    b.prefix = "input.";
    b.boolean("enabled", true);
    b.lin("gainDb", -24, 24, 0, "dB");

    b.prefix = "gate.";
    b.boolean("enabled", false);
    b.lin("thresholdDb", -90, -20, -55, "dB");
    b.lin("hysteresisDb", 0, 10, 4, "dB");
    b.log("attackMs", 0.1f, 10, 0.5f, "ms");
    b.lin("holdMs", 0, 200, 20, "ms");
    b.log("releaseMs", 10, 500, 80, "ms");
    b.lin("rangeDb", -90, 0, -90, "dB");
    b.choice("detector", kDetector, 0, P);
    b.boolean("postAmp", false);

    b.prefix = "comp.";
    b.boolean("enabled", false);
    b.lin("thresholdDb", -50, 0, -20, "dB");
    b.log("ratio", 1, 20, 4, "ratio");
    b.lin("kneeDb", 0, 12, 6, "dB");
    b.log("attackMs", 0.1f, 100, 10, "ms");
    b.log("releaseMs", 20, 1000, 150, "ms");
    b.lin("blendPct", 0, 100, 100, "%");
    b.lin("makeupDb", 0, 24, 0, "dB");
    b.boolean("autoMakeup", true);
    b.choice("mode", kCompMode, 0);
    b.lin("tone", 0, 10, 5, "knob");

    for (const char* drive : {"drive1.", "drive2."}) {
        b.prefix = drive;
        b.boolean("enabled", false);
        b.choice("type", kDriveType, 0);
        b.lin("drive", 0, 10, 2, "knob");
        b.lin("tone", 0, 10, 5, "knob");
        b.lin("levelDb", -20, 12, 0, "dB");
        b.log("lowCutHz", 20, 300, 20, "Hz");
        b.boolean("bright", false);
        b.lin("mixPct", 0, 100, 100, "%");
    }

    b.prefix = "amp.";
    b.choice("channel", kChannel, 0);
    b.lin("cleanBlendPct", 0, 100, 0, "%");
    b.boolean("cabLink", true, P);
    for (int ch = 1; ch <= 3; ++ch) {
        b.prefix = channelPrefix("amp", ch);
        b.lin("gain", 0, 10, ch == 1 ? 3.0f : (ch == 2 ? 5.0f : 6.0f), "knob");
        for (const char* knob : {"bass", "mid", "treble", "presence", "depth", "master"})
            b.lin(knob, 0, 10, 5, "knob");
        b.lin("levelDb", -24, 12, 0, "dB");
        b.boolean("bright", false);
        b.boolean("tight", false);
        b.choice("namLevelMode", kNamLevel, 1, P);
    }

    b.prefix = "cab.";
    b.boolean("enabled", true);
    for (int ch = 1; ch <= 3; ++ch) {
        b.prefix = channelPrefix("cab", ch);
        for (int k = 1; k <= 2; ++k) {
            const std::string ir = "ir" + std::to_string(k) + ".";
            b.boolean(ir + "enabled", k == 1);
            b.lin(ir + "levelDb", -60, 6, 0, "dB");
            b.lin(ir + "pan", -100, 100, 0, "%");
            b.boolean(ir + "invert", false);
            b.lin(ir + "delayMs", 0, 2, 0, "ms");
        }
    }

    b.prefix = "eq.";
    b.boolean("enabled", false);
    b.boolean("followChannel", true, P);
    for (int ch = 1; ch <= 3; ++ch) {
        b.prefix = channelPrefix("eq", ch);
        b.choice("mode", kEqMode, 0, P);
        for (const auto key : kGraphic9)
            b.lin(key, -12, 12, 0, "dB");
        for (const auto key : kGraphic5)
            b.lin(key, -12, 12, 0, "dB");
        constexpr std::array<float, 4> kFreq{100, 500, 2000, 6000};
        for (int k = 1; k <= 4; ++k) {
            const std::string band = "p" + std::to_string(k) + ".";
            b.log(band + "freqHz", 20, 20000, kFreq[static_cast<std::size_t>(k - 1)], "Hz");
            b.lin(band + "gainDb", -15, 15, 0, "dB");
            b.log(band + "q", 0.3f, 6, 0.7f, "ratio");
        }
        b.choice("p1.shape", kEdgeShape, 1, P);
        b.choice("p4.shape", kEdgeShape, 1, P);
        b.log("lowCutHz", 20, 500, 20, "Hz");
        b.log("highCutHz", 2000, 20000, 20000, "Hz");
        b.lin("levelDb", -12, 12, 0, "dB");
    }

    b.prefix = "mod.";
    b.boolean("enabled", false);
    b.choice("type", kModType, 1);
    b.log("rateHz", 0.1f, 10, 0.8f, "Hz");
    b.choice("sync", kModSync, 0);
    b.lin("depthPct", 0, 100, 50, "%");
    b.lin("mixPct", 0, 100, 50, "%");
    b.lin("feedbackPct", -95, 95, 0, "%");
    b.lin("widthPct", 0, 100, 100, "%");
    b.choice("position", kModPosition, 0, P);

    b.prefix = "delay.";
    b.boolean("enabled", false);
    b.choice("mode", kDelayMode, 0);
    b.log("timeMs", 7, 3000, 400, "ms");
    b.choice("sync", kDelaySync, 0);
    b.lin("feedbackPct", 0, 100, 30, "%");
    b.lin("mixPct", 0, 100, 25, "%");
    b.log("lowCutHz", 50, 800, 80, "Hz");
    b.log("highCutHz", 500, 16000, 5000, "Hz");
    b.choice("stereo", kDelayStereo, 0);
    b.lin("modDepthPct", 0, 100, 0, "%");
    b.lin("saturationPct", 0, 100, 20, "%");
    b.lin("duckDb", 0, 24, 0, "dB");

    b.prefix = "reverb.";
    b.boolean("enabled", false);
    b.choice("mode", kReverbMode, 1);
    b.log("decayS", 0.3f, 20, 2.5f, "s");
    b.lin("preDelayMs", 0, 200, 20, "ms");
    b.lin("mixPct", 0, 100, 20, "%");
    b.log("lowCutHz", 50, 700, 100, "Hz");
    b.log("highCutHz", 1000, 10000, 6000, "Hz");
    b.lin("modPct", 0, 100, 20, "%");
    b.lin("shimmerPct", 0, 100, 0, "%");
    b.choice("shimmerInterval", kShimmer, 0);
    b.boolean("freeze", false);

    b.prefix = "transpose.";
    b.boolean("enabled", false, Visibility::reserved);
    b.integer("semitones", -12, 12, 0, "st", Visibility::reserved);

    b.prefix = "doubler.";
    b.boolean("enabled", false);
    b.lin("amountPct", 0, 100, 50, "%");
    b.lin("widthPct", 0, 100, 100, "%");
    b.lin("delayMs", 10, 30, 18, "ms");
    b.lin("detuneCents", 0, 25, 8, "cents");

    b.prefix = "output.";
    b.lin("levelDb", -60, 12, 0, "dB");
    b.boolean("limiter", true);
    return b.table;
}

} // namespace

std::span<const ParamInfo> paramTable()
{
    static const std::vector<ParamInfo> table = buildTable();
    return table;
}

int findParam(std::string_view id)
{
    const auto table = paramTable();
    const auto it = std::find_if(table.begin(), table.end(), [&](const ParamInfo& p) { return p.id == id; });
    return it == table.end() ? -1 : static_cast<int>(it - table.begin());
}

int hostParamCount()
{
    const auto table = paramTable();
    return static_cast<int>(
        std::count_if(table.begin(), table.end(), [](const ParamInfo& p) { return p.vis == Visibility::host; }));
}

float sanitize(const ParamInfo& p, float plain) noexcept OSP_RT_NONBLOCKING
{
    if (std::isnan(plain))
        return p.min;
    const float v = std::clamp(plain, p.min, p.max);
    switch (p.kind) {
    case ParamKind::boolean:
        return v > 0.5f ? 1.0f : 0.0f;
    case ParamKind::choice:
    case ParamKind::integer:
        return std::round(v);
    case ParamKind::continuous:
        break;
    }
    return v;
}

float toNormalized(const ParamInfo& p, float plain) noexcept OSP_RT_NONBLOCKING
{
    const double v = sanitize(p, plain);
    if (p.max <= p.min)
        return 0.0f;
    if (p.skew == Skew::log)
        return static_cast<float>(std::log(v / p.min) / std::log(static_cast<double>(p.max) / p.min));
    return static_cast<float>((v - p.min) / (static_cast<double>(p.max) - p.min));
}

float fromNormalized(const ParamInfo& p, float normalized) noexcept OSP_RT_NONBLOCKING
{
    const double x = std::isnan(normalized) ? 0.0 : std::clamp(static_cast<double>(normalized), 0.0, 1.0);
    if (p.skew == Skew::log)
        return sanitize(p, static_cast<float>(p.min * std::pow(static_cast<double>(p.max) / p.min, x)));
    return sanitize(p, static_cast<float>(p.min + x * (static_cast<double>(p.max) - p.min)));
}

ParamStore::ParamStore()
{
    const auto table = paramTable();
    size_ = static_cast<int>(table.size());
    own_ = std::make_unique<std::atomic<float>[]>(table.size());
    src_.resize(table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
        own_[i].store(table[i].def, std::memory_order_relaxed);
        src_[i] = &own_[i];
    }
}

void ParamStore::set(int index, float plain) noexcept
{
    if (index < 0 || index >= size_)
        return;
    const auto i = static_cast<std::size_t>(index);
    own_[i].store(sanitize(paramTable()[i], plain), std::memory_order_relaxed);
}

bool ParamStore::set(std::string_view id, float plain)
{
    const int index = findParam(id);
    set(index, plain);
    return index >= 0;
}

void ParamStore::bind(int index, const std::atomic<float>* external) noexcept
{
    if (index < 0 || index >= size_)
        return;
    const auto i = static_cast<std::size_t>(index);
    src_[i] = external != nullptr ? external : &own_[i];
}

} // namespace osp::engine
