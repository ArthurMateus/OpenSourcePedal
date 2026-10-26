// osp-render: offline render CLI (ADR-0006 D3, ADR-0007 REQ-TEST-005). It reads a WAV, runs each channel through
// osp::engine::Chain in fixed-size blocks, writes a 32-bit float WAV, and prints the output's integrated loudness
// (ITU-R BS.1770-4 via libebur128) and true peak. JUCE-free. Offline, so allocation here is fine.

#include "osp/engine/Chain.h"

#include <dr_wav.h>
#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string in;
    std::string out;
    float levelDb = 0.0f;
    int block = 64;
};

int usage()
{
    std::fprintf(stderr, "usage: osp-render --in <in.wav> --out <out.wav> [--level-db <dB>] [--block <samples>]\n"
                         "  --level-db  output.levelDb, -60...+12 dB (default 0 = pass-through)\n"
                         "  --block     processing block size, 1...2048 (default 64)\n"
                         "  --preset    not supported yet: it needs the preset loader (engine)\n");
    return 2;
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (v == nullptr)
            return false;
        if (std::strcmp(a, "--in") == 0)
            o.in = v;
        else if (std::strcmp(a, "--out") == 0)
            o.out = v;
        else if (std::strcmp(a, "--level-db") == 0)
            o.levelDb = std::strtof(v, nullptr);
        else if (std::strcmp(a, "--block") == 0)
            o.block = std::atoi(v);
        else
            return false;
        ++i;
    }
    return !o.in.empty() && !o.out.empty() && o.block >= 1 && o.block <= 2048 && o.levelDb >= -60.0f &&
           o.levelDb <= 12.0f;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--preset") == 0) {
            std::fprintf(stderr, "osp-render: --preset is not supported yet (needs the engine preset loader)\n");
            return 2;
        }
    if (!parse(argc, argv, opt))
        return usage();

    unsigned int channels = 0;
    unsigned int rate = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(opt.in.c_str(), &channels, &rate, &frames, nullptr);
    if (raw == nullptr || channels == 0) {
        std::fprintf(stderr, "osp-render: cannot read %s\n", opt.in.c_str());
        return 1;
    }
    std::vector<float> data(raw, raw + frames * channels);
    drwav_free(raw, nullptr);

    // Deinterleave, process each channel with its own chain, interleave again.
    const auto n = static_cast<std::size_t>(frames);
    std::vector<float> mono(n);
    for (unsigned int c = 0; c < channels; ++c) {
        for (std::size_t i = 0; i < n; ++i)
            mono[i] = data[i * channels + c];
        osp::engine::Chain chain;
        chain.prepare(static_cast<double>(rate), opt.block);
        for (std::size_t pos = 0; pos < n; pos += static_cast<std::size_t>(opt.block)) {
            const auto len = std::min(static_cast<std::size_t>(opt.block), n - pos);
            chain.setOutputLevelDb(opt.levelDb);
            chain.process(mono.data() + pos, mono.data() + pos, static_cast<int>(len));
        }
        for (std::size_t i = 0; i < n; ++i)
            data[i * channels + c] = mono[i];
    }

    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = channels;
    fmt.sampleRate = rate;
    fmt.bitsPerSample = 32;
    drwav wav;
    if (!drwav_init_file_write(&wav, opt.out.c_str(), &fmt, nullptr)) {
        std::fprintf(stderr, "osp-render: cannot write %s\n", opt.out.c_str());
        return 1;
    }
    const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, data.data());
    drwav_uninit(&wav);
    if (written != frames) {
        std::fprintf(stderr, "osp-render: short write to %s\n", opt.out.c_str());
        return 1;
    }

    // Loudness of the output. Default channel map (channel 0 = left, weight 1), the same as pyloudnorm for mono.
    ebur128_state* st = ebur128_init(channels, rate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    double lufs = -HUGE_VAL;
    double truePeak = 0.0;
    if (st != nullptr && ebur128_add_frames_float(st, data.data(), static_cast<size_t>(frames)) == EBUR128_SUCCESS) {
        ebur128_loudness_global(st, &lufs);
        for (unsigned int c = 0; c < channels; ++c) {
            double tp = 0.0;
            if (ebur128_true_peak(st, c, &tp) == EBUR128_SUCCESS)
                truePeak = std::max(truePeak, tp);
        }
    }
    ebur128_destroy(&st);

    std::printf("in:        %s (%u ch, %u Hz, %llu frames)\n", opt.in.c_str(), channels, rate,
                static_cast<unsigned long long>(frames));
    std::printf("out:       %s (32-bit float, block %d, level %+.2f dB)\n", opt.out.c_str(), opt.block, opt.levelDb);
    std::printf("LUFS-I:    %.2f\n", lufs);
    std::printf("TruePeak:  %.2f dBTP\n", truePeak > 0.0 ? 20.0 * std::log10(truePeak) : -HUGE_VAL);
    return 0;
}
