// osp-tonematch: offline tone match CLI (ADR-0008). Profiles the guitar in a reference recording (WAV or MP3), fits
// a chain setting to the player's own DI with osp::engine::tonematch, and writes a preset JSON plus, on request, the
// DI rendered through that setting. YouTube links are resolved by tools/tonematch/tonematch.py, which calls this.

#include "osp/dsp/conv/IrLoader.h"
#include "osp/engine/ToneMatch.h"

#include <dr_mp3.h>
#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace tmatch = osp::engine::tonematch;

namespace {

struct Audio {
    std::vector<float> interleaved;
    unsigned channels = 0;
    unsigned rate = 0;
    std::size_t frames() const { return channels == 0 ? 0 : interleaved.size() / channels; }
};

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool load(const std::string& path, Audio& a)
{
    drwav_uint64 frames = 0;
    float* raw = nullptr;
    if (lower(fs::path(path).extension().string()) == ".mp3") {
        drmp3_config cfg{};
        drmp3_uint64 n = 0;
        raw = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &n, nullptr);
        a.channels = cfg.channels;
        a.rate = cfg.sampleRate;
        frames = n;
        if (raw != nullptr) {
            a.interleaved.assign(raw, raw + frames * a.channels);
            drmp3_free(raw, nullptr);
        }
    } else {
        raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &a.channels, &a.rate, &frames, nullptr);
        if (raw != nullptr) {
            a.interleaved.assign(raw, raw + frames * a.channels);
            drwav_free(raw, nullptr);
        }
    }
    return raw != nullptr && a.channels > 0 && a.rate > 0;
}

std::vector<float> channel(const Audio& a, unsigned c, std::size_t from, std::size_t to)
{
    std::vector<float> out;
    out.reserve(to - from);
    for (std::size_t i = from; i < to; ++i)
        out.push_back(a.interleaved[i * a.channels + std::min(c, a.channels - 1)]);
    return out;
}

std::string fileUri(const fs::path& p)
{
    std::string s = fs::absolute(p).generic_string();
    return "file:///" + (s.starts_with("/") ? s.substr(1) : s);
}

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

int usage()
{
    std::fprintf(stderr, "usage: osp-tonematch --ref <song.wav|mp3> --di <my-di.wav> --out <preset.json> [options]\n"
                         "  --start/--end <s>   analyse only this part of the reference (pick a guitar-heavy section)\n"
                         "  --source <s>        auto (default), mono, mid, side, left, right\n"
                         "  --no-hpss           keep percussive energy (use for an isolated guitar stem)\n"
                         "  --ir-dir <dir>      cab IR folder (default: the bundled assets/cab)\n"
                         "  --render <out.wav>  also write the DI through the matched setting\n"
                         "  --name <text>       preset name (default: Tone match)\n");
    return 2;
}

void printBands(const tmatch::Profile& ref, const tmatch::Profile& got)
{
    std::printf("\n  band      ref    match   diff\n");
    for (int k = 0; k < tmatch::kBands; k += 2) {
        if (tmatch::fitWeight(k) == 0.0)
            continue;
        const double d = got.levelDb[k] - ref.levelDb[k];
        std::printf("  %6.0f Hz %+6.1f %+6.1f %+6.1f  ", tmatch::bandHz(k), ref.levelDb[k], got.levelDb[k], d);
        const int bars = std::min(20, static_cast<int>(std::lround(std::abs(d) * 2.0)));
        for (int i = 0; i < bars; ++i)
            std::putchar(d > 0 ? '+' : '-');
        std::putchar('\n');
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string refPath, diPath, outPath, renderPath, irDir = OSP_ASSET_DIR "/cab", name = "Tone match";
    double start = 0.0, end = -1.0;
    tmatch::Source source = tmatch::Source::automatic;
    tmatch::MatchOptions opt;
    bool factoryIrs = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-hpss") {
            opt.analysis.hpss = false;
            continue;
        }
        if (i + 1 >= argc)
            return usage();
        const std::string v = argv[++i];
        if (a == "--ref")
            refPath = v;
        else if (a == "--di")
            diPath = v;
        else if (a == "--out")
            outPath = v;
        else if (a == "--render")
            renderPath = v;
        else if (a == "--name")
            name = v;
        else if (a == "--start")
            start = std::atof(v.c_str());
        else if (a == "--end")
            end = std::atof(v.c_str());
        else if (a == "--ir-dir") {
            irDir = v;
            factoryIrs = false;
        } else if (a == "--source") {
            if (v == "auto")
                source = tmatch::Source::automatic;
            else if (v == "mono")
                source = tmatch::Source::mono;
            else if (v == "mid")
                source = tmatch::Source::mid;
            else if (v == "side")
                source = tmatch::Source::side;
            else if (v == "left")
                source = tmatch::Source::left;
            else if (v == "right")
                source = tmatch::Source::right;
            else
                return usage();
        } else
            return usage();
    }
    if (refPath.empty() || diPath.empty() || outPath.empty())
        return usage();

    try {
        Audio ref, di;
        if (!load(refPath, ref)) {
            std::fprintf(stderr, "osp-tonematch: cannot read %s\n", refPath.c_str());
            return 1;
        }
        if (!load(diPath, di)) {
            std::fprintf(stderr, "osp-tonematch: cannot read %s\n", diPath.c_str());
            return 1;
        }
        const std::size_t from = std::min(ref.frames(), static_cast<std::size_t>(std::max(0.0, start) * ref.rate));
        const std::size_t to =
            end > start ? std::min(ref.frames(), static_cast<std::size_t>(end * ref.rate)) : ref.frames();
        if (to <= from) {
            std::fprintf(stderr, "osp-tonematch: empty --start/--end range\n");
            return 1;
        }
        const auto left = channel(ref, 0, from, to);
        const auto right = channel(ref, 1, from, to);
        tmatch::Source used = tmatch::Source::mono;
        const auto refMono =
            pickSource(left.data(), ref.channels > 1 ? right.data() : nullptr, left.size(), ref.rate, source, &used);
        const tmatch::Profile refProfile = tmatch::analyse(refMono, ref.rate, opt.analysis);

        // The DI is taken as mono (channel 0) at its own rate; IRs are conditioned to that rate.
        const auto diMono = channel(di, 0, 0, di.frames());
        const double fsDi = di.rate;
        std::vector<tmatch::CabIr> cabs;
        std::vector<fs::path> irFiles;
        for (const auto& e : fs::directory_iterator(irDir))
            if (e.is_regular_file() && lower(e.path().extension().string()) == ".wav")
                irFiles.push_back(e.path());
        std::sort(irFiles.begin(), irFiles.end());
        for (const auto& p : irFiles) {
            Audio irA;
            if (!load(p.string(), irA))
                continue;
            const auto raw = channel(irA, 0, 0, irA.frames());
            tmatch::CabIr c;
            c.name = p.stem().string();
            c.uri = factoryIrs ? "factory:ir/" + c.name : fileUri(p);
            c.ir = osp::dsp::IrLoader::condition(raw.data(), static_cast<int>(raw.size()), irA.rate, fsDi);
            cabs.push_back(std::move(c));
        }
        if (cabs.empty()) {
            std::fprintf(stderr, "osp-tonematch: no .wav IRs in %s\n", irDir.c_str());
            return 1;
        }

        std::printf("reference: %s (%u ch, %u Hz), %.1f…%.1f s, source %s, %.1f s of active audio%s\n", refPath.c_str(),
                    ref.channels, ref.rate, static_cast<double>(from) / ref.rate, static_cast<double>(to) / ref.rate,
                    tmatch::toString(used), refProfile.activeSeconds,
                    opt.analysis.hpss ? ", drums removed (HPSS)" : "");
        std::printf("DI:        %s (%u Hz, %.1f s), %zu cab IRs\n", diPath.c_str(), di.rate,
                    static_cast<double>(diMono.size()) / fsDi, cabs.size());
        std::printf("searching drive x cab x EQ ...\n");

        const tmatch::MatchResult r = tmatch::match(refProfile, diMono, fsDi, cabs, opt);
        const auto& s = r.setting;

        std::printf("\nmatched setting\n");
        std::printf("  input     %+.1f dB\n", s.inputGainDb);
        if (s.drive == tmatch::DriveKind::clean)
            std::printf("  drive1    off (clean)\n");
        else
            std::printf("  drive1    %s, drive %.1f, tight %s\n", tmatch::toString(s.drive), s.driveKnob,
                        s.tightHz > 20.0f ? (std::to_string(static_cast<int>(s.tightHz)) + " Hz").c_str() : "off");
        std::printf("  cab       %s\n", cabs[static_cast<std::size_t>(s.cab)].name.c_str());
        std::printf("  eq        63 %+.1f | 125 %+.1f | 250 %+.1f | 500 %+.1f | 1k %+.1f | 2k %+.1f | 4k %+.1f | "
                    "8k %+.1f | 16k %+.1f dB\n",
                    s.eqDb[0], s.eqDb[1], s.eqDb[2], s.eqDb[3], s.eqDb[4], s.eqDb[5], s.eqDb[6], s.eqDb[7], s.eqDb[8]);
        std::printf("            low cut %s, high cut %s\n",
                    s.eqLowCutHz > 20.0f ? (std::to_string(static_cast<int>(s.eqLowCutHz)) + " Hz").c_str() : "off",
                    s.eqHighCutHz < 20000.0f ? (std::to_string(static_cast<int>(s.eqHighCutHz)) + " Hz").c_str()
                                             : "off");
        std::printf("  output    %+.1f dB\n", s.outputDb);
        std::printf("\nflatness (dB)  500-1k  1k-2k  2k-4k  4k-8k\n");
        std::printf("  reference   %6.1f %6.1f %6.1f %6.1f\n", r.reference.flatnessDb[0], r.reference.flatnessDb[1],
                    r.reference.flatnessDb[2], r.reference.flatnessDb[3]);
        std::printf("  match       %6.1f %6.1f %6.1f %6.1f\n", r.matched.flatnessDb[0], r.matched.flatnessDb[1],
                    r.matched.flatnessDb[2], r.matched.flatnessDb[3]);
        printBands(r.reference, r.matched);
        std::printf("\nspectral residual: %.2f dB (your DI through a flat clean chain: %.2f dB)\n", r.residualDb,
                    r.unmatchedResidualDb);
        std::printf("runners-up:\n");
        for (std::size_t i = 1; i < r.ranking.size(); ++i) {
            const auto& c = r.ranking[i].setting;
            std::printf("  %s %.0f tight %.0f, cab %s (predicted residual %.2f dB, flatness %.2f dB)\n",
                        tmatch::toString(c.drive), c.driveKnob, c.tightHz,
                        cabs[static_cast<std::size_t>(c.cab)].name.c_str(), r.ranking[i].residualDb,
                        r.ranking[i].flatnessDistance);
        }

        std::ofstream(outPath, std::ios::binary) << tmatch::toPresetJson(r, cabs, name, makeUuid());
        std::printf("\npreset:  %s\n", outPath.c_str());

        if (!renderPath.empty()) {
            const auto out = tmatch::render(diMono, fsDi, s, cabs);
            drwav_data_format fmt{};
            fmt.container = drwav_container_riff;
            fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
            fmt.channels = 1;
            fmt.sampleRate = di.rate;
            fmt.bitsPerSample = 32;
            drwav wav;
            if (!drwav_init_file_write(&wav, renderPath.c_str(), &fmt, nullptr)) {
                std::fprintf(stderr, "osp-tonematch: cannot write %s\n", renderPath.c_str());
                return 1;
            }
            drwav_write_pcm_frames(&wav, out.size(), out.data());
            drwav_uninit(&wav);
            std::printf("render:  %s\n", renderPath.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "osp-tonematch: %s\n", e.what());
        return 1;
    }
    return 0;
}
