#pragma once

// Spring tank of the Reverb block (issue #73): the parametric spring reverberation model of V. Välimäki, J. Parker and
// J. S. Abel, "Parametric Spring Reverberation Effect", J. Audio Eng. Soc. 58(7/8), 2010, as summarised in J. Parker,
// "Efficient Dispersion Generation Structures for Spring Reverb Emulation", EURASIP J. Adv. Signal Process.
// 2011:646134, §2, eqs. (1)-(3) and Fig. 1 (open access, https://doi.org/10.1155/2011/646134, accessed 2026-10-27).
// Pure C++20, no allocation outside prepare(). Same interface as FdnTank and DattorroTank, so the block can hold any of
// them.
//
// Structure, per spring (two springs, L and R, with different lengths, both fed the mono sum (L + R)/2):
// - C_lf, the main chirp series below the transition frequency f_C: feedback loop of a cascade of M_lf = 100
//   "stretched" first-order allpasses A(z^k) = (a + z^−k)/(1 + a·z^−k) with k = round(fs / (2·f_C)) and a = +0.6, so
//   the group delay peaks at f_C (Parker §2: "The group delay has a maximum at fC"), a delay line (transit time, read
//   with LFO modulation), a one-pole damping low-pass and the decay gain. Its output is low-passed at f_C (2nd-order
//   SVF), as in the paper, to suppress the aliased group-delay curve above f_C.
// - C_hf, the weaker wide-band chirp series: the same loop with M_hf = 40 unstretched allpasses (k = 1) and a = −0.6,
//   so the group delay is largest at low frequencies and falls with frequency (Parker §2), mixed in at −14 dB.
// The paper's cross-coupling of the two structures, its random delay modulation and its fractional stretch factor are
// left out: the LFO is a sine, and k is an integer, so f_C is 4.41 kHz at 44.1 kHz, 4.8 kHz at 48 kHz, 4.41 kHz at
// 88.2 kHz and 4.36 kHz at 96 kHz (spring units sit at about 2-5 kHz, Parker §3; ours is a fixed voicing). The a, M,
// transit times and output levels are our voicing choices (*unverified* against the paper's tables, which could not
// be read here).
//
// Decay: the parameter is the mid-band RT60 (T_mid, as in FdnTank). Each loop's gain is g = 10^(−3·T/(fs·RT60)),
// with T = the delay length plus the cascade's exact group delay at 707 Hz. The in-loop damping is Jot's one-pole
// (RT60 at fs/2 = half the RT60 at DC; see the .cpp), with its loss at 707 Hz compensated. Freeze ramps both loops to
// lossless (gains 1, damping and modulation off) and mutes the input over 50 ms; the allpasses are lossless, so the
// frozen tail holds its level.

#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

#include <array>
#include <vector>

namespace osp::dsp {

class SpringTank {
public:
    static constexpr int kLfStages = 100;
    static constexpr int kHfStages = 40;
    static constexpr double kTransitionHz = 4400.0; // f_C before rounding k

    // Non-RT. Allocates every buffer at this rate.
    void prepare(double sampleRate);
    void reset() noexcept;

    // RT-safe, once per block. Same semantics as FdnTank, including the NaN-safe clamps (NaN → the minimum).
    void setDecaySeconds(float seconds) noexcept { decay_.setTarget(clampSafe(seconds, 0.3f, 20.0f)); }
    void setModulation(float amount) noexcept { mod_.setTarget(clampSafe(amount, 0.0f, 1.0f)); }
    void setFreeze(bool freeze) noexcept { freeze_.setTarget(freeze ? 1.0f : 0.0f); }

    // RT-safe. Wet only. in and out buffers must not alias each other.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    int stretch() const noexcept { return stretch_; } // k of the C_lf allpasses (tests)

private:
    static float clampSafe(float v, float lo, float hi) noexcept { return v > lo ? (v < hi ? v : hi) : lo; }

    struct Line {
        std::vector<float> buf;
        int write = 0;
        void push(float x) noexcept
        {
            buf[static_cast<std::size_t>(write)] = x;
            if (++write == static_cast<int>(buf.size()))
                write = 0;
        }
        float frac(float delay) const noexcept; // delay >= 1, linear interpolation
    };
    struct Spring {
        std::vector<float> lfState; // kLfStages rings of stretch_ samples, sharing one index
        std::array<float, kHfStages> hfState{};
        Line lfDelay, hfDelay;
        int lfLength = 1, hfLength = 1;
        float lfDamp = 0.0f, hfDamp = 0.0f, lfFb = 0.0f, hfFb = 0.0f, lfGain = 0.0f, hfGain = 0.0f, lfPole = 0.0f,
              hfPole = 0.0f;
        double lfLoop = 1.0, hfLoop = 1.0; // loop times at 707 Hz, samples
        SvfState outLp;
    };

    void updateGains() noexcept;

    double sampleRate_ = 0.0;
    int stretch_ = 1, lfIndex_ = 0;
    std::array<Spring, 2> springs_;
    SvfCoeffs outLp_{};
    float excursion_ = 0.0f;
    double lfoPhase_ = 0.0, lfoInc_ = 0.0;
    float cachedDecay_ = -1.0f;
    OnePoleSmoother decay_, mod_;
    LinearRamp freeze_;
};

} // namespace osp::dsp
