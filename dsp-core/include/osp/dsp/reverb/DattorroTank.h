#pragma once

// Plate tank of the Reverb block (issue #72, part 3): the figure-of-eight allpass/delay tank of J. Dattorro, "Effect
// Design, Part 1: Reverberator and Other Filters", J. Audio Eng. Soc. 45(9), 1997, §1.3 (Fig. 1, Tables 1 and 2).
// Pure C++20, no allocation outside prepare(). Same interface as FdnTank, so the block can hold either.
//
// Structure: (L + R)/2 → bandwidth one-pole → 4 input allpass diffusers (142, 107, 379, 277 samples at the paper's
// 29761 Hz) → two cross-coupled tank halves: modulated allpass (672 / 908) → delay (4453 / 4217) → damping one-pole
// → decay gain → allpass (1800 / 2656) → delay (3720 / 3163) → decay gain → the other half's input. L and R are sums
// of 7 signed taps into the right-hand and left-hand delays and allpasses (Table 2). All lengths scale with fs.
//
// Provenance of the numbers: the lengths and the 12 tap offsets they share were cross-checked against CAPS 0.9.26
// Reverb.cc (T. Goetze, GPL-3.0-or-later; numbers only, no code), which stores them in seconds and gives exactly
// these values at 29761 Hz. The 7th tap per side (1066 into the left 3720 delay, 121 into the right 3163 delay), the
// tap signs, and the coefficients (bandwidth 0.9995, input diffusion 0.75 / 0.625, decay diffusion 0.70 / 0.50,
// damping 0.0005) follow the paper; the scanned paper could not be read here, so those are *unverified*. CAPS uses
// 6 taps per side and a 12-sample excursion at 29761 Hz, which is the maximum used here (at 100 % modulation).
//
// Decay: the paper's "decay" is a raw loop gain. Here, as in FdnTank, the parameter is the mid-band RT60 (ISO 3382-1
// T_mid): four decay gains per loop of T_loop = 21589 samples at 29761 Hz (0.725 s), g = 10^(−3·T_loop/(4·RT60)),
// with the damping loss at 707 Hz compensated. A loop that long makes very short decays grainy (at 0.3 s one pass is
// −60 dB), so the plate is specified for 1…20 s; the block clamps nothing extra, the measured RT60 is just coarser.
// Freeze ramps to a lossless loop (gains 1, damping and modulation off) and mutes the input over 50 ms.

#include "osp/dsp/util/Smoothers.h"

#include <array>
#include <vector>

namespace osp::dsp {

class DattorroTank {
public:
    // Non-RT. Allocates every delay buffer at this rate.
    void prepare(double sampleRate);
    void reset() noexcept;

    // RT-safe, once per block. Same semantics as FdnTank.
    void setDecaySeconds(float seconds) noexcept { decay_.setTarget(seconds); }
    void setModulation(float amount) noexcept { mod_.setTarget(amount); }
    void setFreeze(bool freeze) noexcept { freeze_.setTarget(freeze ? 1.0f : 0.0f); }

    // RT-safe. Wet only. in and out buffers must not alias each other.
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
        float at(int delay) const noexcept; // delay >= 1: at(1) is the most recent push
        float frac(float delay) const noexcept;
    };
    // Schroeder allpass on a Line of `length`: w = x + g·d, y = d − g·w, push w.
    struct Allpass {
        Line line;
        int length = 1;
        float process(float x, float g) noexcept;
        float processModulated(float x, float g, float delay) noexcept;
    };

    void updateGains() noexcept;

    double sampleRate_ = 0.0;
    double scale_ = 1.0; // fs / 29761
    std::array<Allpass, 4> diffuser_;
    Allpass modL_, modR_, apL_, apR_;
    Line delayL1_, delayL2_, delayR1_, delayR2_;
    int lenL1_ = 1, lenL2_ = 1, lenR1_ = 1, lenR2_ = 1;
    std::array<int, 14> taps_{};
    float excursion_ = 0.0f;
    double lfoPhase_ = 0.0, lfoInc_ = 0.0;
    float bandwidthState_ = 0.0f, dampL_ = 0.0f, dampR_ = 0.0f, fbL_ = 0.0f, fbR_ = 0.0f;
    float gain_ = 0.0f, gainDamped_ = 0.0f, cachedDecay_ = -1.0f;
    double dampCompensation_ = 1.0;
    OnePoleSmoother decay_, mod_;
    LinearRamp freeze_;
};

} // namespace osp::dsp
