#pragma once

// Reverb block (issue #72): room and hall on FdnTank (part 1), plate on DattorroTank (part 3). Stereo in/out, zero
// latency (the dry path is never delayed). Parameters per parameters.md §3.5.
//
// Signal path: in → pre-delay (fractional, 80 ms glide) → low cut / high cut (TPT one-pole HP/LP on the reverb
// input, cutoff smoothed 20 ms, coefficients per sample while moving) → tank (mode engine, 50 ms crossfade on a mode
// change, one fade at a time: a change during a fade starts when it ends) → wet. out = dry·(1 − m·e) + wet·m, with m =
// mix (20 ms ramp) and e = the enable ramp (10 ms). Disabled = soft bypass with trails (the tank input is muted, the
// tail rings out, signal-flow.md §5); setHardBypass is the exact-copy bypass used by null tests (ADR-0007
// REQ-TEST-004). TPT one-pole: V. Zavalishin, "The Art of VA Filter Design", rev. 2.1.2, 2018, §3.
//
// Mode fade: the incoming engine starts cold (reset), so the wet level dips while its tail builds up and the outgoing
// tail fades out over the same 50 ms. Accepted: a warm start would need every engine running all the time.
// Denormals: the cut filters' and tanks' decaying states rely on FTZ/DAZ at callback entry (realtime-rules RT-8),
// with no per-state snap.

#include "osp/dsp/reverb/DattorroTank.h"
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
        float decayS = 2.5f; // 0.3…20
        float preDelayMs = 20.0f; // 0…200
        float mixPct = 20.0f; // 0…100
        float lowCutHz = 100.0f; // 50…700
        float highCutHz = 6000.0f; // 1000…10000
        float modPct = 20.0f; // 0…100
        bool freeze = false;
    };

    Reverb() noexcept { hard_.reset(true); } // processed, not hard-bypassed, by default

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParameters(const Params& p) noexcept; // RT-safe, once per block; clamps (NaN → minimum)
    void setEnabled(bool enabled) noexcept; // soft bypass (trails)
    void setHardBypass(bool bypass) noexcept; // exact dry copy once settled

    // RT-safe. inL/inR may alias outL/outR. Any n >= 0. Before prepare() it zero-fills.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    int getLatencySamples() const noexcept { return 0; }

private:
    struct Tpt1 { // one-pole TPT state per channel
        float s[2] = {0.0f, 0.0f};
    };

    void startPendingModeFade() noexcept;
    void resetEngine(int engine) noexcept;
    void processEngine(int engine, const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;
    void processChunk(const float* inL, const float* inR, float* outL, float* outR, int n) noexcept;

    double sampleRate_ = 0.0;
    Params params_;
    static constexpr int kNumEngines = 3;
    std::array<FdnTank, 2> engines_; // 0 = room, 1 = hall
    DattorroTank plate_;             // 2 = plate
    int active_ = 1, previous_ = 1, pending_ = 1;
    LinearRamp modeFade_;
    std::array<std::vector<float>, 2> pre_; // pre-delay rings
    int preWrite_ = 0;
    OnePoleSmoother preDelay_; // samples, 80 ms glide
    OnePoleSmoother lowCut_, highCut_;
    Tpt1 hp_, lp_;
    float gHp_ = 0.0f, gLp_ = 0.0f, cachedLow_ = -1.0f, cachedHigh_ = -1.0f;
    LinearRamp mix_, enabled_;
    Crossfader hard_;
    std::array<std::vector<float>, 7> scratch_; // tank in L/R, wet L/R, previous-engine wet L/R, enable ramp
};

} // namespace osp::dsp
