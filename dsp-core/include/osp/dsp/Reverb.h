#pragma once

// Reverb block, part 1 of 2 (issue #72): room and hall on FdnTank; plate uses the hall engine until part 2 adds the
// Dattorro tank. Stereo in/out, zero latency (the dry path is never delayed). Parameters per parameters.md §3.5.
//
// Signal path: in → pre-delay (fractional, 80 ms glide) → low cut / high cut (TPT one-pole HP/LP on the reverb
// input, cutoff smoothed 20 ms, coefficients per sample while moving) → tank (mode engine, 50 ms crossfade on a mode
// change) → wet. out = dry·(1 − m·e) + wet·m, with m = mix (20 ms ramp) and e = the enable ramp (10 ms).
// Disabled = soft bypass with trails (the tank input is muted, the tail rings out, signal-flow.md §5); setHardBypass
// is the exact-copy bypass used by null tests (ADR-0007 REQ-TEST-004).
// TPT one-pole: V. Zavalishin, "The Art of VA Filter Design", rev. 2.1.2, 2018, §3.

#include "osp/dsp/reverb/FdnTank.h"
#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/Smoothers.h"

#include <array>
#include <vector>

namespace osp::dsp {

class Reverb {
public:
    enum class Mode { Room, Hall, Plate };

    struct Params {
        Mode mode = Mode::Hall;
        float decayS = 2.5f;      // 0.3…20
        float preDelayMs = 20.0f; // 0…200
        float mixPct = 20.0f;     // 0…100
        float lowCutHz = 100.0f;  // 50…700
        float highCutHz = 6000.0f; // 1000…10000
        float modPct = 20.0f;     // 0…100
        bool freeze = false;
    };

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParameters(const Params& p) noexcept; // RT-safe, once per block; clamps to the parameters.md ranges
    void setEnabled(bool enabled) noexcept;       // soft bypass (trails)
    void setHardBypass(bool bypass) noexcept;      // exact dry copy once settled

    // RT-safe. inL/inR may alias outL/outR. Any n >= 0. Before prepare() it zero-fills.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    int getLatencySamples() const noexcept { return 0; }

private:
    struct Tpt1 { // one-pole TPT state per channel
        float s[2] = {0.0f, 0.0f};
    };

    void processChunk(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    double sampleRate_ = 0.0;
    Params params_;
    std::array<FdnTank, 2> engines_; // 0 = room, 1 = hall (plate → hall in part 1)
    int active_ = 1, previous_ = 1;
    LinearRamp modeFade_;
    std::array<std::vector<float>, 2> pre_; // pre-delay rings
    int preWrite_ = 0;
    OnePoleSmoother preDelay_;   // samples, 80 ms glide
    OnePoleSmoother lowCut_, highCut_;
    Tpt1 hp_, lp_;
    float gHp_ = 0.0f, gLp_ = 0.0f, cachedLow_ = -1.0f, cachedHigh_ = -1.0f;
    LinearRamp mix_, enabled_;
    Crossfader hard_;
    std::array<std::vector<float>, 7> scratch_; // tank in L/R, wet L/R, previous-engine wet L/R, enable ramp
};

} // namespace osp::dsp
