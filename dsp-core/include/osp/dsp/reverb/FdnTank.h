#pragma once

// 8-channel feedback delay network with a multichannel input diffuser: the room and hall engines of the Reverb
// block (time-based-effects.md §1.1, §2.1; issue #72). Pure C++20, no allocation outside prepare().
//
// Structure: stereo in → D diffuser stages (8 channels, per-channel delays, 8×8 Hadamard, polarity flips) → 8 delay
// lines with modulated fractional reads (linear interpolation) → per-line gentle one-pole damping (10 kHz) and decay
// gain g_i = 10^(−3·D_i / (fs·RT60)) / |H_damp(707 Hz)| → 8×8 Householder feedback (I − (2/8)·11ᵀ) → back into the
// lines. L = Σ even lines, R = Σ odd lines (alternating signs), so the two outputs decorrelate. Sources: J.-M. Jot & A.
// Chaigne, "Digital Delay Networks for Designing Artificial Reverberators", AES 90th Conv., 1991 (FDN, RT60 gains); J.
// O. Smith, "Physical Audio Signal Processing", FDN reverberation chapter (CCRMA, accessed 2026-10-25); G. Luff, "Let's
// Write a Reverb", ADC 2021 (multichannel Hadamard diffuser and Householder feedback; the structure only, no code: the
// example repository has no licence, time-based-effects.md §3). The decay parameter is the mid-band RT60 (ISO 3382-1
// T_mid, 500 Hz and 1 kHz octaves): the damping loss at 707 Hz is compensated, so higher frequencies decay faster, as
// in a real room. Freeze ramps the loop to lossless (g = 1, damping and modulation off) and mutes the input over 50 ms.

#include "osp/dsp/util/Smoothers.h"

#include <array>
#include <vector>

namespace osp::dsp {

class FdnTank {
public:
    static constexpr int kChannels = 8;

    struct Config {
        int diffuserStages = 4;
        double diffuserMs = 80.0; // total spread of the diffuser, split evenly over the stages
        double lineMinMs = 35.0; // FDN delay lengths, geometric between min and max, rounded to primes
        double lineMaxMs = 110.0;
        double modDepthSamples48k = 12.0; // at 100 % modulation, scaled with fs
        double lfoHz = 0.4;
    };

    // The two FDN voicings of the Reverb block (time-based-effects.md §2.1). Room: shorter lines (20–60 ms) for a
    // small-space character; 4 diffuser stages keep the tail free of ringing even at long decays (the 2-stage,
    // 15–45 ms draft measured local flatness 0.73, PR description). Hall: 35–110 ms lines, deeper modulation.
    static constexpr Config kRoom{4, 30.0, 20.0, 60.0, 8.0, 0.6};
    static constexpr Config kHall{4, 80.0, 35.0, 110.0, 12.0, 0.4};

    // Non-RT. Allocates every delay buffer for this config at this rate.
    void prepare(double sampleRate, const Config& config);
    void reset() noexcept;

    // RT-safe, once per block. decay 0.3…20 s (smoothed 20 ms), mod 0…1 (smoothed 20 ms), freeze ramps 50 ms.
    void setDecaySeconds(float seconds) noexcept { decay_.setTarget(seconds); }
    void setModulation(float amount) noexcept { mod_.setTarget(amount); }
    void setFreeze(bool freeze) noexcept { freeze_.setTarget(freeze ? 1.0f : 0.0f); }

    // RT-safe. Wet only (no dry). in and out buffers must not alias each other.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

private:
    struct Line {
        std::vector<float> buf;
        int write = 0;
        void push(float x) noexcept
        {
            buf[static_cast<std::size_t>(write)] = x;
            if (++write == static_cast<int>(buf.size()))
                write = 0;
        }
        float at(int delay) const noexcept; // integer delay ≥ 1
        float frac(float delay) const noexcept; // fractional delay ≥ 1, linear interpolation
    };

    void updateGains() noexcept;

    double sampleRate_ = 0.0;
    double lfoInc_ = 0.0;
    float modDepth_ = 0.0f;
    std::vector<std::array<Line, kChannels>> diffuser_;
    std::vector<std::array<int, kChannels>> diffuserDelay_;
    std::array<Line, kChannels> lines_;
    std::array<int, kChannels> length_{};
    std::array<float, kChannels> gain_{};
    std::array<float, kChannels> lp_{};
    std::array<double, kChannels> lfoPhase_{};
    float dampCoeff_ = 0.0f;
    double dampCompensation_ = 1.0; // 1 / |H_damp| at the RT60 reference frequency
    float cachedDecay_ = -1.0f;
    OnePoleSmoother decay_, mod_;
    LinearRamp freeze_;
};

} // namespace osp::dsp
