#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Chorus ×2 and flanger, the modulated-delay types of `mod.*` (parameters.md §3.3, issue #70, part 1/3). Stereo in,
// stereo out, zero latency (signal-flow §4, L_mod = 0: the dry path is never delayed, no through-zero). The `mod`
// block (part 3) crossfades between two instances on a type change and sums to mono in the pre-amp position.
// Algorithm: modulated fractional delay lines, D(t) = D0 + M·LFO(t) (J. Dattorro, "Effect Design, Part 2: Delay-Line
// Modulation and Chorus", JAES 45(10), 1997, https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf; J. O. Smith,
// PASP, "Flanging", https://ccrma.stanford.edu/~jos/pasp/Flanging.html; both accessed 2026-10-27), read with the
// Catmull-Rom tap of util/Lfo.h. A delay changing at dτ/dt shifts pitch by 1 − dτ/dt, so a sine sweep of ±M seconds at
// f Hz gives a peak deviation of 2πfM. Voicings (own choices inside time-based-effects §2.4/§3.1 ranges):
// - chorus80s: one voice per side, D0 7 ms, M up to 3 ms, the wet one-pole low-passed at 5 kHz (darker, BBD-like).
// - chorusWide: two voices per side half a cycle apart, D0 12 ms, M up to 4 ms, mixed at 0.5 each.
// - flanger: one voice per side, τ = 0.25 ms + depth · 4 ms · (1 + LFO) / 2, bipolar feedback `feedbackPct`.
// Width sets the right side's LFO phase offset: 0…180° (chorus80s, flanger) or 0…90° (chorusWide, whose voices are
// already 180° apart). Feedback applies to the flanger only. Mix: out = dry + mix·(wet − dry).
// Smoothing (parameters.md §1.4): mix, width and feedback are 20 ms linear ramps, depth a 20 ms one-pole and the rate
// a 20 ms one-pole inside the Lfo. Enable: a 10 ms ramp of the mix, so disabled is the exact dry signal.

#include "osp/dsp/util/Lfo.h"
#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp {

class ChorusFlanger {
public:
    enum class Voicing { chorus80s, chorusWide, flanger };

    static constexpr float kMinRateHz = 0.1f, kMaxRateHz = 10.0f, kMaxFeedbackPct = 95.0f;
    static constexpr float kFlangerMinMs = 0.25f, kFlangerSweepMs = 4.0f;
    // Feedback-loop state limit: never reached with |input| <= 1, it only bounds pathological input.
    static constexpr float kLoopLimit = 8.0f;

    struct VoicingSpec {
        float baseMs, depthMs, wetLowPassHz; // wetLowPassHz 0 = none
        int voices;
        float maxWidthCycles;
    };
    static VoicingSpec spec(Voicing v) noexcept;

    // Starts at the parameters.md defaults: chorusWide, 0.8 Hz, depth 50 %, mix 50 %, feedback 0, width 100 %; enabled.
    ChorusFlanger() noexcept;

    // Non-RT: allocates the two rings for the longest voicing.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the rings, filters and LFO phase, and jumps every smoother to its target.
    void reset() noexcept;

    // RT-safe. Clamped to the parameters.md ranges; NaN gives the range minimum. A voicing change is a jump: the
    // `mod` block crossfades two instances instead (xf 30).
    void setVoicing(Voicing v) noexcept { voicing_ = v; }
    void setRateHz(float hz) noexcept;
    void setDepthPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setFeedbackPct(float pct) noexcept;
    void setWidthPct(float pct) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on both channels. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    Voicing voicing() const noexcept { return voicing_; }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return lineL_.isPrepared(); }

private:
    double sampleRate_ = 0.0;
    Voicing voicing_ = Voicing::chorusWide;
    bool bypassed_ = false;
    Lfo lfo_;
    LinearRamp mix_{0.5f}, width_{1.0f}, feedback_{0.0f}, enabled_{1.0f};
    OnePoleSmoother depth_;
    ModDelayLine lineL_, lineR_;
    float lpL_ = 0.0f, lpR_ = 0.0f;
};

} // namespace osp::dsp
