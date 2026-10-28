#pragma once

// Shimmer ("dreamscape") tank of the Reverb block (issue #73, time-based-effects.md §2.2): the hall FdnTank with a
// pitch-shifted regeneration loop around it. Pure C++20, no allocation outside prepare(). Same interface as the other
// tanks, plus the shimmer amount and interval (parameters.md §3.5: reverb.shimmerPct, reverb.shimmerInterval).
//
// Loop, per channel: tank wet → 2-pole SVF low-pass at 4.5 kHz → GrainShifter (40 ms grains, WSOLA-aligned splices;
// octUp ×2, fifthUp ×2^(7/12), octDown ×0.5; the right channel 2 cents above the left and the left 2 cents below, for
// width) → the same low-pass again → 2-pole SVF high-pass at 100 Hz → two Schroeder allpass diffusers (7 and 11 ms,
// g = 0.65) → tanh(r · s) → added to the tank input, with the return r = amount · 0.6 · min(1, √(2.5 s / decay)).
// These follow the four rules of time-based-effects.md §2.2 (steep HF roll-off after the shifter, post-shifter
// diffusion, L/R detune, a DC blocker + tanh), with three measured changes (PR description):
// - The DC blocker is a 100 Hz 2-pole high-pass: the splices put energy at the grain rate (25 Hz for an octave), which
//   a 20 Hz one-pole passed, and at 8-20 s decay the tail grew about 2 dB/s after the input stopped.
// - The √ in r: the tank's amplitude gain for a sustained input grows as √RT60, so it keeps the loop gain flat.
// - The low-pass before the shifter keeps content above fs/4 from folding back when it is read at 2× speed.
// The diffusers are not modulated. Each pass moves the energy up (or down) by the interval, so the low-pass (or the
// high-pass) ends every cascade, and tanh bounds the return whatever the tank holds.
//
// The loop closes through a 32-sample ring: the tank runs in chunks of at most 32 samples, and its input at sample n
// takes the shimmer path's output from sample n − 32. That adds 0.7 ms to the 20 ms mean grain delay and keeps the
// result independent of the host block size. Freeze mutes the tank's input, the shimmer return included, so a frozen
// tail holds its level and does not climb. An interval change dips the shimmer return to 0 over 25 ms, switches the
// ratio, and ramps it back over 25 ms (the spec's xf 50, as a dip: one shifter per channel).

#include "osp/dsp/reverb/FdnTank.h"
#include "osp/dsp/reverb/GrainShifter.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

#include <array>
#include <vector>

namespace osp::dsp {

class ShimmerTank {
public:
    enum class Interval { OctUp, FifthUp, OctDown };
    static constexpr int kLoopChunk = 32;

    // Non-RT. Allocates every buffer at this rate.
    void prepare(double sampleRate);
    void reset() noexcept;

    // RT-safe, once per block. NaN-safe clamps (NaN → the minimum), as in FdnTank.
    void setDecaySeconds(float seconds) noexcept
    {
        decay_ = seconds > 0.3f ? (seconds < 20.0f ? seconds : 20.0f) : 0.3f;
        tank_.setDecaySeconds(decay_);
        updateReturn();
    }
    void setModulation(float amount) noexcept { tank_.setModulation(amount); }
    void setFreeze(bool freeze) noexcept { tank_.setFreeze(freeze); }
    void setShimmer(float amount) noexcept
    {
        amount_ = amount > 0.0f ? (amount < 1.0f ? amount : 1.0f) : 0.0f;
        updateReturn();
    }
    void setInterval(Interval interval) noexcept { pending_ = interval; }

    // RT-safe. Wet only. in and out buffers must not alias each other.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    static double ratio(Interval interval) noexcept;

private:
    struct Allpass {
        std::vector<float> buf;
        int write = 0;
        float process(float x, float g) noexcept;
    };
    struct Path {
        GrainShifter shifter;
        SvfState preLp, lp, hp;
        std::array<Allpass, 2> diffuser;
        std::array<float, kLoopChunk> ring{};
    };

    void applyInterval() noexcept;
    void updateReturn() noexcept;
    void processChunk(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    double sampleRate_ = 0.0;
    FdnTank tank_;
    std::array<Path, 2> path_;
    SvfCoeffs lp_{}, hp_{};
    int ringPos_ = 0;
    Interval current_ = Interval::OctUp, pending_ = Interval::OctUp;
    float amount_ = 0.0f, decay_ = 2.5f;
    OnePoleSmoother return_; // the loop's return gain, before tanh (20 ms)
    LinearRamp intervalGain_;
    std::array<std::array<float, kLoopChunk>, 2> in_{};
};

} // namespace osp::dsp
