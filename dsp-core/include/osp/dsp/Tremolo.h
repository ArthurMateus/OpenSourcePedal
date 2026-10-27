#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Harmonic tremolo, the `tremolo` type of `mod.*` (parameters.md §3.3, issue #70, part 3/4). Stereo in, stereo out,
// zero latency (L_mod = 0). The signal is split at kCrossoverHz into a low band and its complement, and the two bands
// are amplitude-modulated in antiphase (U. Zölzer (ed.), "DAFX: Digital Audio Effects", 2nd ed., Wiley 2011, ch. 2
// (tremolo as amplitude modulation); time-based-effects §2.4: crossover at 600 Hz, bands 180° apart):
// - low = two cascaded TPT one-pole low-passes at 600 Hz (the Linkwitz-Riley 2nd-order low band; Zavalishin, "The Art
//   of VA Filter Design" rev. 2.1.0 ch. 3, https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/
//   VAFilterDesign_2.1.0.pdf, accessed 2026-10-27), high = x − low. The bands are complementary, so with the two
//   gains equal the output is exactly the input (no allpass phase, unlike low − high of a true LR crossover).
// - gLow = 1 − depth·(1 + LFO)/2, gHigh = 1 − depth·(1 − LFO)/2, out = gLow·low + gHigh·high. At depth 100 % each
//   band is fully silenced once per LFO cycle, half a cycle apart. Own choice: one crossover, no bias/plain mode.
// Width sets the right side's LFO phase offset (0…180°, a stereo "pan" tremolo at 100 %). Mix: out = dry + mix·(wet −
// dry), as in the other types; feedback is ignored. Smoothing and enable as ChorusFlanger: mix and width 20 ms linear
// ramps, depth and rate 20 ms one-poles, enable a 10 ms ramp of the mix, so disabled is the exact dry signal.

#include "osp/dsp/util/Lfo.h"
#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp {

class Tremolo {
public:
    static constexpr float kMinRateHz = 0.1f, kMaxRateHz = 10.0f;
    static constexpr double kCrossoverHz = 600.0;

    // Starts at the parameters.md defaults: 0.8 Hz, depth 50 %, mix 50 %, width 100 %; enabled.
    Tremolo() noexcept;

    // Non-RT (computes coefficients only, no allocation).
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the crossover and the LFO phase, and jumps every smoother to its target.
    void reset() noexcept;

    // RT-safe. Clamped to the parameters.md ranges; NaN gives the range minimum.
    void setRateHz(float hz) noexcept;
    void setDepthPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setWidthPct(float pct) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on both channels. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    // The band gain laws above for an LFO value in [−1, 1] and a depth in [0, 1], shared with the tests.
    static float lowGain(float lfo, float depth) noexcept { return 1.0f - depth * 0.5f * (1.0f + lfo); }
    static float highGain(float lfo, float depth) noexcept { return 1.0f - depth * 0.5f * (1.0f - lfo); }

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

private:
    struct Side {
        float s1 = 0.0f, s2 = 0.0f;
    };
    float processSide(Side& side, float x, double phase, float depth) const noexcept;

    double sampleRate_ = 0.0;
    float G_ = 0.0f; // TPT one-pole g / (1 + g) at kCrossoverHz
    bool bypassed_ = false;
    Lfo lfo_;
    LinearRamp mix_{0.5f}, width_{1.0f}, enabled_{1.0f};
    OnePoleSmoother depth_;
    Side left_, right_;
};

} // namespace osp::dsp
