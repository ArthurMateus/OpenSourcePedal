#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Doubler, `doubler.*` in parameters.md §3.6 (Global-Lock), REQ-418: stereo double-tracking from short modulated
// delays, zero added latency (signal-flow §4, L_dbl = 0: the dry path is never delayed). Stereo in, stereo out, after
// the reverb.
// - Two voices: A reads the left input, B the right, each through a fractional delay of `delayMs` (10...30 ms) that a
//   slow sine LFO modulates. A delay that changes at rate dtau/dt shifts the pitch by the factor 1 - dtau/dt, so a
//   depth of D seconds at f Hz gives a peak deviation of 2 pi f D: D = (2^(cents/1200) - 1) / (2 pi f), with cents =
//   `detuneCents`. The LFOs run at kRateAHz and kRateBHz (own choice: slow, unrelated, starting in opposite phase), so
//   the two takes never drift in step.
// - Width: voice A is panned left and B right by `widthPct` (linear pan, centred at 0 %).
// - Amount: out = dry + amount x kWetGain x (panned voices); kWetGain puts each voice 3 dB under the dry at 100 %.
// - Smoothing (parameters.md §1.4): amount and width are mixes, 20 ms linear ramps; delayMs is a time, an 80 ms
//   one-pole glide of the read position (tape-style); detuneCents a 20 ms one-pole on the depth. Enable: 10 ms (xf 10)
//   ramp of the wet path only, so disabled is the exact dry signal.
// - Read: 4-point cubic (Catmull-Rom) interpolation (P. Bourke, "Interpolation methods", 1999,
//   https://paulbourke.net/miscellaneous/interpolation/, accessed 2026-10-26). The delay PR #116 adds a shared
//   FractionalDelay; once it is on main this block should use it.

#include "osp/dsp/util/Smoothers.h"

#include <array>
#include <vector>

namespace osp::dsp {

class Doubler {
public:
    static constexpr float kMinDelayMs = 10.0f, kMaxDelayMs = 30.0f;
    static constexpr float kMaxDetuneCents = 25.0f;
    static constexpr double kRateAHz = 0.53, kRateBHz = 0.71;
    static constexpr float kWetGain = 0.70710678f;

    // Starts at the parameters.md defaults: amount 50 %, width 100 %, 18 ms, 8 cents; enabled.
    Doubler() noexcept;

    // Non-RT: allocates the two delay rings.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the rings and LFO phases and jumps every smoother to its target: renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Clamped to the parameters.md ranges; NaN gives the range minimum.
    void setAmountPct(float pct) noexcept;
    void setWidthPct(float pct) noexcept;
    void setDelayMs(float ms) noexcept;
    void setDetuneCents(float cents) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on both channels. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return !ringL_.empty(); }

private:
    float read(const std::vector<float>& ring, double delaySamples) const noexcept;

    double sampleRate_ = 0.0;
    bool bypassed_ = false;
    LinearRamp amount_{0.5f}, width_{1.0f}, enabled_{1.0f};
    OnePoleSmoother delayMs_, detune_;
    std::vector<float> ringL_, ringR_;
    unsigned mask_ = 0, write_ = 0;
    double phaseA_ = 0.0, phaseB_ = 0.5; // in cycles
};

} // namespace osp::dsp
