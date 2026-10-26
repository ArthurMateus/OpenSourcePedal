#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Cab block, `cab.*` in parameters.md §3.1: two mono IR slots, each with enable, level, pan, phase invert and an
// alignment delay, summed to stereo (signal-flow §3: mono in → stereo out). Latency 0 (L_cab = 0, signal-flow §4).
//
// IR objects follow ADR-0004 §4: the worker builds and prepare()s a PartitionedConvolver at the processing rate,
// the audio thread installs it with setIr() at a block boundary and fades from the old one over the ADR-0004 swap
// time. The block never deletes a convolver: every object it lets go of (faded out, or superseded before it was
// installed) is returned by popRetired() for the retire queue. setIr() retires at most one object and process() at
// most one per slot, so draining popRetired() after each of them keeps the kRetireCapacity ring from overflowing.
//
// CPU peak: the 1024-sample stage of a long IR does one 2048-point FFT pair plus its whole delay-line sum in the
// callback that completes a frame. The block staggers those callbacks (PR #93 review): slot k's convolvers run
// with frame phases in [512·k, 512·k + 512), and each swap moves the incoming one 256 samples away from the one
// it replaces, so the two slots, and an IR fading in, never complete a 1024 frame in the same callback. A slot that
// was skipped (off, or block bypassed) is reset and re-phased when it comes back, which also drops its stale input.
//
// Own decisions (parameters.md leaves them open):
// - Pan law: constant power, normalised to unity at centre, gL = √2·cos θ, gR = √2·sin θ, θ = (pan + 1)·π/4. The
//   default (centre) keeps the mono gain staging on both channels; a hard pan is +3 dB on one side.
// - Align delay: Catmull-Rom (cubic Hermite) interpolation for delays >= 1 sample, linear below 1 sample. Both are
//   exact at integer delays, so the default 0 ms is bit-exact. Source: Catmull-Rom/Hermite interpolation as in
//   Signalsmith `dsp` delay.h (MIT) and P. Bourke, "Interpolation methods", 1999,
//   https://paulbourke.net/miscellaneous/interpolation/ (accessed 2026-10-26).

#include "osp/dsp/conv/PartitionedConvolver.h"
#include "osp/dsp/util/Crossfader.h"
#include "osp/dsp/util/Smoothers.h"

#include <array>
#include <vector>

namespace osp::dsp {

class Cab {
public:
    static constexpr int kNumSlots = 2;
    static constexpr double kSwapSeconds = 0.025; // ADR-0004 §5 owns this value
    static constexpr float kMaxDelayMs = 2.0f;
    static constexpr int kRetireCapacity = 8;

    Cab() noexcept;

    // Non-RT. Allocates the scratch buffers and the align delay lines. The installed IRs are kept.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Finishes any IR fade at once, clears the delay lines and every installed convolver, and jumps every
    // smoother to its target: two renders after reset() are bit-identical.
    void reset() noexcept;

    // RT-safe. `ir` is prepared at this sample rate (or nullptr = slot silent). Ownership stays with the engine.
    void setIr(int slot, PartitionedConvolver* ir) noexcept;
    // RT-safe. One object to retire per call, nullptr when there is none.
    PartitionedConvolver* popRetired() noexcept;
    // RT-safe. Hands every object back (teardown); the slots are silent afterwards.
    void retireAll() noexcept;

    // RT-safe, once per block. Values are clamped to the parameters.md ranges.
    void setEnabled(bool on) noexcept
    {
        enableL_.setTarget(on);
        enableR_.setTarget(on);
    }
    void setSlotEnabled(int slot, bool on) noexcept;
    void setLevelDb(int slot, float dB) noexcept;
    void setPanPct(int slot, float pct) noexcept;
    void setInvert(int slot, bool on) noexcept;
    void setDelayMs(int slot, float ms) noexcept;
    // Hard bypass for tests (signal-flow §5): both outputs = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. Mono in, stereo out; `in` may alias outL (not outR). Before prepare() it zero-fills the outputs.
    void process(const float* in, float* outL, float* outR, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return maxBlock_ > 0; }
    bool isSwapping(int slot) const noexcept;

private:
    struct Slot {
        PartitionedConvolver* active = nullptr;
        PartitionedConvolver* incoming = nullptr; // fading in
        PartitionedConvolver* pending = nullptr;  // arrived during a fade, installed after it
        Crossfader swap;                          // 25 ms, a = active, b = incoming
        LinearRamp enabled;                       // xf 10, 0…1
        LinearRamp level{1.0f};                   // 20 ms, linear gain
        LinearRamp sign{1.0f};                    // xf 10, +1 / −1
        LinearRamp pan;                           // 20 ms, −1…+1
        LinearRamp delay;                         // 20 ms, samples
        float delayMs = 0.0f;
        std::vector<float> history;               // align delay ring, power-of-two length
        int write = 0;
        int phase = 0;      // frame phase offset of the active convolver (see the CPU peak note)
        bool stale = false; // skipped since the last render: reset and re-phase before the next one
    };

    void retire(PartitionedConvolver* ir) noexcept;
    // `ahead`: samples between clock_ and the first sample the new IR will process (PR #100 review: a pending IR
    // promoted at the end of a chunk starts with the next chunk).
    void startSwap(Slot& s, PartitionedConvolver* ir, int ahead = 0) noexcept;
    void completeSwap(Slot& s, int ahead = 0) noexcept;
    void renderSlot(Slot& s, const float* in, float* outL, float* outR, int n) noexcept;
    int slotBase(const Slot& s) const noexcept;
    int phaseNow(int phase, int ahead = 0) const noexcept;

    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    bool bypassed_ = false;
    std::array<Slot, kNumSlots> slots_;
    Crossfader enableL_, enableR_; // block enable, xf 10: a = dry (input on both sides), b = wet
    std::vector<float> mono_, incoming_, wetL_, wetR_;
    std::array<PartitionedConvolver*, kRetireCapacity> retired_{};
    int retiredCount_ = 0;
    unsigned clock_ = 0; // samples processed since reset(), modulo 2^32 (only its value mod 1024 is used)
};

} // namespace osp::dsp
