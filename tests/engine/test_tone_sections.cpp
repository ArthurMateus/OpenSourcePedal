// Guitar parts and tone sections (ADR-0008 D5), on synthetic songs with a known answer built from the bundled CC0
// DI: guitar 1 (left) plays clean, crunch, clean again with different excerpts; guitar 2 (right) plays fuzz through
// another cab the whole time. A double-tracked guitar must stay one part.

#include "osp/dsp/conv/IrLoader.h"
#include "osp/engine/ToneMatch.h"
#include "osp/engine/ToneSections.h"

#include <catch2/catch_test_macros.hpp>

#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace tmatch = osp::engine::tonematch;

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

std::vector<tmatch::CabIr> cabs(double fs)
{
    std::vector<tmatch::CabIr> out;
    for (const char* n : {"412-a", "412-e"}) {
        unsigned rate = 0;
        const auto raw = readMono(std::string(OSP_ASSET_DIR) + "/cab/" + n + ".wav", rate);
        out.push_back({n, std::string("factory:ir/") + n, "",
                       osp::dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), rate, fs)});
    }
    return out;
}

// `seconds` of the DI starting at `from`, through setting s, peak-normalised to 0.5.
std::vector<float> part(const std::vector<float>& di,
                        double fs,
                        double from,
                        double seconds,
                        const tmatch::Setting& s,
                        const std::vector<tmatch::CabIr>& c)
{
    const auto a = static_cast<std::size_t>(from * fs), b = a + static_cast<std::size_t>(seconds * fs);
    auto y = tmatch::render(std::vector<float>(di.begin() + static_cast<std::ptrdiff_t>(a),
                                               di.begin() + static_cast<std::ptrdiff_t>(std::min(b, di.size()))),
                            fs, s, c);
    float peak = 0.0f;
    for (float v : y)
        peak = std::max(peak, std::abs(v));
    for (auto& v : y)
        v *= 0.5f / peak;
    return y;
}

void append(std::vector<float>& to, const std::vector<float>& x)
{
    to.insert(to.end(), x.begin(), x.end());
}

} // namespace

TEST_CASE("tonematch: two guitars, one changing tone, are split into parts and tones", "[tonematch][slow]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const double fs = rate;
    const auto c = cabs(fs);
    tmatch::Setting clean, crunch, fuzz;
    crunch.drive = tmatch::DriveKind::dist;
    crunch.driveKnob = 6.0f;
    fuzz.drive = tmatch::DriveKind::fuzz;
    fuzz.driveKnob = 8.0f;
    fuzz.cab = 1;
    fuzz.eqDb = {0, 0, -4, -4, 0, 4, 4, 0, 0};

    std::vector<float> left, right;
    append(left, part(di, fs, 0.0, 8.0, clean, c));  // tone 1
    append(left, part(di, fs, 1.0, 8.0, crunch, c)); // tone 2
    append(left, part(di, fs, 2.5, 8.0, clean, c));  // tone 1 again, other notes
    for (double from : {0.5, 2.0, 1.5})
        append(right, part(di, fs, from, 8.0, fuzz, c));
    REQUIRE(left.size() == right.size());

    const auto r = tmatch::findSections(left.data(), right.data(), left.size(), fs);
    REQUIRE(r.parts.size() == 2);
    CHECK(r.parts[0].source == tmatch::Source::left);
    int leftTones = 0, rightTones = 0;
    for (const auto& s : r.sections) {
        std::string ranges;
        for (const auto& t : s.ranges)
            ranges += " " + std::to_string(t.startSeconds) + "-" + std::to_string(t.endSeconds);
        WARN("guitar " << s.part + 1 << ", tone " << s.tone + 1 << ":" << ranges);
        (s.part == 0 ? leftTones : rightTones)++;
    }
    CHECK(leftTones == 2);
    CHECK(rightTones == 1);

    // The tone cap: with at most one tone per part, guitar 1's clean and crunch merge into one section over 0-24 s.
    tmatch::SectionOptions one;
    one.maxTones = 1;
    const auto capped = tmatch::findSections(left.data(), right.data(), left.size(), fs, one);
    REQUIRE(capped.sections.size() == 2);
    for (const auto& s : capped.sections) {
        CHECK(s.tone == 0);
        CHECK(std::abs(s.seconds - 24.0) < 0.01);
    }

    // Guitar 1, tone 1: 0-8 s and 16-24 s; tone 2: 8-16 s, each boundary within one hop (2 s).
    for (const auto& s : r.sections) {
        if (s.part != 0)
            continue;
        if (s.tone == 0) {
            REQUIRE(s.ranges.size() == 2);
            CHECK(std::abs(s.ranges[0].endSeconds - 8.0) <= 2.0);
            CHECK(std::abs(s.ranges[1].startSeconds - 16.0) <= 2.0);
        } else {
            REQUIRE(s.ranges.size() == 1);
            CHECK(std::abs(s.ranges[0].startSeconds - 8.0) <= 2.0);
            CHECK(std::abs(s.ranges[0].endSeconds - 16.0) <= 2.0);
        }
    }

    // Each tone matches back to a setting of its own kind: the crunch and fuzz sections to driven ones.
    const tmatch::Matcher m(di, fs, c);
    for (const auto& s : r.sections) {
        const auto res = m.match(s.profile);
        WARN("guitar " << s.part + 1 << ", tone " << s.tone + 1 << ": " << tmatch::toString(res.setting.drive) << " "
                       << res.setting.driveKnob << ", cab " << c[static_cast<std::size_t>(res.setting.cab)].name
                       << ", residual " << res.residualDb << " dB");
        // The clean sections are excerpts while the matcher uses the whole DI (with its ring-out), so their envelope
        // spread differs a little and the match may add a light boost: clean, or transparent/mid at drive <= 2, counts
        // as low gain (measured: transparent 2, 0.28 dB). The crunch and fuzz sections must come out driven harder.
        const auto& st = res.setting;
        const bool lowGain = st.drive == tmatch::DriveKind::clean ||
                             ((st.drive == tmatch::DriveKind::transparent || st.drive == tmatch::DriveKind::mid) &&
                              st.driveKnob <= 2.0f);
        CHECK(lowGain == (s.part == 0 && s.tone == 0));
        if (s.part == 1)
            CHECK(res.setting.cab == 1);
    }
}

TEST_CASE("tonematch: a double-tracked guitar stays one part", "[tonematch]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const double fs = rate;
    const auto c = cabs(fs);
    tmatch::Setting crunch;
    crunch.drive = tmatch::DriveKind::mid;
    crunch.driveKnob = 5.0f;
    const auto l = part(di, fs, 0.0, 9.0, crunch, c);
    const auto r = part(di, fs, 0.6, 9.0, crunch, c); // the "second take": same tone, other timing
    const auto res = tmatch::findSections(l.data(), r.data(), std::min(l.size(), r.size()), fs);
    CHECK(res.parts.size() == 1);
    CHECK(res.sections.size() == 1);
}

TEST_CASE("tonematch: a hard-panned guitar with bleed, or a silent side, is one part", "[tonematch]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const double fs = rate;
    const auto c = cabs(fs);
    tmatch::Setting crunch;
    crunch.drive = tmatch::DriveKind::mid;
    crunch.driveKnob = 5.0f;
    const auto l = part(di, fs, 0.0, 9.0, crunch, c);
    const float bleed = std::pow(10.0f, -40.0f / 20.0f); // #121 review: per-side gates made this "guitar 2"
    std::vector<float> r(l.size());
    for (std::size_t i = 0; i < l.size(); ++i)
        r[i] = bleed * l[i];
    const auto withBleed = tmatch::findSections(l.data(), r.data(), l.size(), fs);
    CHECK(withBleed.parts.size() == 1);
    std::fill(r.begin(), r.end(), 0.0f);
    const auto silentSide = tmatch::findSections(l.data(), r.data(), l.size(), fs);
    CHECK(silentSide.parts.size() == 1);
    for (const auto& s : silentSide.sections)
        CHECK(s.part == 0);
}

TEST_CASE("tonematch: findSections edge cases: mono, silence, short and off-grid input", "[tonematch]")
{
    unsigned rate = 0;
    const auto di = readMono(std::string(OSP_ASSET_DIR) + "/di/strum-arp-01.wav", rate);
    const double fs = rate;

    // Mono: one part, labelled mono.
    const auto mono = tmatch::findSections(di.data(), nullptr, di.size(), fs);
    REQUIRE(mono.parts.size() == 1);
    CHECK(mono.parts[0].source == tmatch::Source::mono);

    // Silence throws, as documented.
    const std::vector<float> silent(static_cast<std::size_t>(5.0 * fs), 0.0f);
    CHECK_THROWS_AS(tmatch::findSections(silent.data(), nullptr, silent.size(), fs), std::invalid_argument);

    // Shorter than one window: one window, one tone over the whole input.
    const auto shortRes = tmatch::findSections(di.data(), nullptr, static_cast<std::size_t>(3.0 * fs), fs);
    REQUIRE(shortRes.sections.size() == 1);
    CHECK(std::abs(shortRes.sections[0].seconds - 3.0) < 1e-6);

    // Off the hop grid (10.75 s with 4 s windows, 2 s hop) and no cap: the ranges of all tones tile the input once,
    // with no overlap, and sectionAudio returns exactly the ranges' length.
    tmatch::SectionOptions noCap;
    noCap.maxTones = 0;
    const auto off = tmatch::findSections(di.data(), nullptr, di.size(), fs, noCap);
    std::vector<tmatch::TimeRange> all;
    double total = 0.0;
    for (const auto& s : off.sections) {
        all.insert(all.end(), s.ranges.begin(), s.ranges.end());
        total += s.seconds;
        const auto audio = tmatch::sectionAudio(di.data(), nullptr, di.size(), fs, off, s);
        CHECK(std::abs(static_cast<double>(audio.size()) / fs - s.seconds) < 2.0 / fs * s.ranges.size());
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    for (std::size_t i = 1; i < all.size(); ++i)
        CHECK(all[i].startSeconds >= all[i - 1].endSeconds - 1e-9);
    CHECK(total <= static_cast<double>(di.size()) / fs + 1e-6);
}
