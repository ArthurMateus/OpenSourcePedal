#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Phaser and vibe, the allpass types of `mod.*` (parameters.md §3.3, issue #70, part 2/3). Stereo in, stereo out,
// zero latency (L_mod = 0). Each side is a cascade of first-order TPT (zero-delay-feedback) allpass stages,
// y = 2·LP − x with the trapezoidal one-pole LP, g = tan(π fc / fs), coefficients updated every sample (V. Zavalishin,
// "The Art of VA Filter Design" rev. 2.1.0, ch. 3 and 5, https://www.native-instruments.com/fileadmin/ni_media/
// downloads/pdf/VAFilterDesign_2.1.0.pdf; J. O. Smith, PASP, "Phasing with First-Order Allpass Filters",
// https://ccrma.stanford.edu/~jos/pasp/Phasing_First_Order_Allpass_Filters.html; both accessed 2026-10-27). A stage's
// phase is −2·atan(tan(πf/fs) / g), so mixing the cascade with the dry signal notches where the total phase is an odd
// multiple of π. Voicings (own choices inside time-based-effects §2.4/§3.1):
// - phaser: 6 equal stages (3 notches), fc = 120 Hz · (3200/120)^(½ + ½·depth·LFO), so depth 0 parks it at the
//   geometric centre (620 Hz); global feedback `feedbackPct` (±95 %) from the cascade output, one sample delayed.
// - vibe: 4 staggered stages (fc × 1, 1.8, 3.4, 6), swept 100 Hz · 16^(depth·cell). `cell` is a lamp-and-photocell
//   model: the LFO drives a lamp at (½ + ½·LFO)², and a one-pole with a 5 ms attack and a 25 ms release follows it,
//   which gives the swept, uneven throb of the photocell units (shape *unverified* against hardware). No feedback.
// Width sets the right side's LFO phase offset (0…180°). Mix: out = dry + mix·(wet − dry); 50 % gives the deepest
// notches, 100 % on the vibe gives pitch vibrato. Smoothing as ChorusFlanger: mix, width and feedback are 20 ms linear
// ramps, depth and rate 20 ms one-poles; enable is a 10 ms ramp of the mix, so disabled is the exact dry signal.

#include "osp/dsp/util/Lfo.h"
#include "osp/dsp/util/Smoothers.h"

#include <array>

namespace osp::dsp {

class Phaser {
public:
    enum class Voicing { phaser, vibe };

    static constexpr int kPhaserStages = 6, kVibeStages = 4, kMaxStages = 6;
    static constexpr float kMinRateHz = 0.1f, kMaxRateHz = 10.0f, kMaxFeedbackPct = 95.0f;
    static constexpr double kPhaserLowHz = 120.0, kPhaserHighHz = 3200.0;
    static constexpr double kVibeLowHz = 100.0, kVibeSpan = 16.0;
    static constexpr std::array<double, kVibeStages> kVibeRatios{1.0, 1.8, 3.4, 6.0};

    // Starts at the parameters.md defaults: 0.8 Hz, depth 50 %, mix 50 %, feedback 0, width 100 %; phaser; enabled.
    Phaser() noexcept;

    // Non-RT (computes coefficients only, no allocation).
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the stages, the photocells and the LFO phase, and jumps every smoother to its target.
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

    // The phaser's static centre frequency (depth 0), for the notch tests.
    static double phaserCentreHz() noexcept;

    Voicing voicing() const noexcept { return voicing_; }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

private:
    struct Side {
        std::array<float, kMaxStages> s{};
        float last = 0.0f, cell = 0.0f;
    };
    float processSide(Side& side, float x, double lfo, float depth, float fb) noexcept;

    double sampleRate_ = 0.0;
    Voicing voicing_ = Voicing::phaser;
    bool bypassed_ = false;
    float cellAttack_ = 1.0f, cellRelease_ = 1.0f;
    Lfo lfo_;
    LinearRamp mix_{0.5f}, width_{1.0f}, feedback_{0.0f}, enabled_{1.0f};
    OnePoleSmoother depth_;
    Side left_, right_;
};

} // namespace osp::dsp
