#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// One triode preamp gain stage (ADR-0011 D7 slice P5-5; docs/research/classic-amp-models.md §2, §4). It runs at the
// rate given to prepare(), which is the OS-2 oversampled rate inside a classic amp (ADR-0005 D2; OS-2 is #174).
//
// Signal path per sample, in normalised grid units (x = 1 is Vgk = 0, i.e. the bias distance Vk0 of the circuit):
//   drive gain -> coupling cap with grid conduction (bias shift) -> Miller low-pass -> cathode shelf
//   -> static triode curve f with 2nd-order ADAA -> output gain (inverting by default)
//
// Design record (decisions taken here, with what was measured; all circuit values are textbook, unverified):
// - Static curve (research Q2). The research default, an asymmetric rational soft clip with closed-form F1, was fitted
//   to the offline load line (tools/amp/preamp_fit.py) and rejected: its best fit misses the curve by 0.20 (normalised)
//   and by up to 10.4 dB THD (curve 1.82 % vs fit 5.36 % at peak 0.25). The triode curve is expansive below the grid
//   onset (3/2-power law) and compresses only towards cutoff, which no rational soft clip follows. So this uses the
//   research's table fallback (§2.1): prepare() solves the Koren load line (N. Koren, "Improved vacuum tube models for
//   SPICE simulations", https://www.normankoren.com/Audio/Tubemodspice_article_2.html, accessed 2026-10-28; the
//   article's 12AX7 constants mu 100, Ex 1.4, Kg1 1060, Kp 600, Kvb 300) for B+ 250 V, Rp 100 kΩ, Rk 1.5 kΩ
//   AC-bypassed, with the grid current as a static clamp (Koren's diode behind RGI = 2 kΩ, fed from Rs; quadratic
//   knee of ±0.1 V). The table holds f on [−4, 4] in 1/128 steps. f is piecewise linear; F1 and F2 are its exact
//   piecewise-quadratic and -cubic antiderivatives in double, so ADAA sees a consistent set. Above x = 4 (the plate
//   heading to saturation) a rational tail with closed-form F1, F2 matches f(4), the end slope and f(64); below −4
//   (cutoff) f is extended linearly. Measured against the Python reference: 2.0e-6 max error on [−3, 4], 0.057 (3.6 %)
//   in the fitted tail at x = 16. f is normalised to f'(0) = 1; circuitGainDb() is the circuit's 35.65 dB.
// - ADAA2 (Bilbao, Esqueda, Parker, Välimäki, IEEE SPL 24(7), 2017; the formula and fallbacks of osp::dsp::Adaa2),
//   not the ADAA1 of ADR-0011 D3: at +40 dB drive the curve clips hard on both sides, and ADAA1 at 4x measured
//   −48.6 dB ASR at 2.5 kHz (gate −60) and −41.9 dB at 5 kHz (gate −40), 44.1 kHz, 0 dBFS. ADAA2 gives −68.2 and −61.7
//   (research Q9: escalate when a gate fails; ADR-0005 D2 gives hard-clip shapes 2nd order). Because F2 grows like x²,
//   the ill-conditioning tolerance is 1e-5·(1 + |x|). Delay 1 sample at the stage rate.
// - Grid conduction / bias shift (research §2.3, Q3). The coupling cap C sees the leak path (Rs + Rg) always and the
//   grid-current path (Rs + RGI) when the grid is above the onset (x − v_c > 1): dv_c/dt = ω_r·(x − v_c) +
//   ω_a·max(x − v_c − 1, 0). The release is not a separate parameter: it is the coupling RC itself, τ_r =
//   1/(2π·couplingHz) (12 Hz -> 13.3 ms, inside the research's 10–50 ms). Attack τ_a = τ_r·(Rs + RGI)/(Rs + Rg)
//   (68 kΩ, 2 kΩ, 1 MΩ: 0.87 ms at 12 Hz, inside 0.3–2 ms). Both regimes are TPT one-poles (Zavalishin, "The Art of
//   VA Filter Design", 2018, §3.10); the linear one runs first and the conducting one replaces it if the grid ends
//   above the onset. Q3 (a SPICE run) is not done: no ngspice on this machine, so these times are unverified.
// - Cathode bypass (research §2.3) is a first-order low shelf before the curve, not after it: the unbypassed cathode
//   resistor lowers the gain into the plate curve, so bass clips later (the "tight" low end). A post-curve shelf would
//   only re-equalise an already clipped signal. Depth in Config (0 dB = fully bypassed), corner smoothed.
// - Smoothing (parameters.md §1.4; ADR-0011 D3). The drive is a 30 ms one-pole on the linear pre-gain. Filter corners
//   are 20 ms one-poles on the TPT coefficient G = g/(1 + g) itself: setters compute the target G (one tan) at most
//   once per call and only when the value changes, and the filters take the smoothed G every sample, as §1.4
//   requires (a 16-sample update failed REQ-TEST-003 in #87). Renders are bit-identical across block sizes.
// - The stage inverts (a common-cathode stage does), so a cascade alternates which side of the next curve clips.
//
// Latency: kDelaySamples (ADAA) plus the filters' group delay; getMeasuredDelaySamples() returns the 1 kHz group delay
// of the small-signal path at the current corners, at the stage rate. Integer host latency is the model's job (#174).

#include "osp/dsp/util/Smoothers.h"

#include <array>

namespace osp::dsp::amp {

struct PreampConfig {
    double supplyVolts = 250.0; // B+
    double plateOhms = 100.0e3; // Rp
    double cathodeOhms = 1.5e3; // Rk: sets the bias point, and with it the curve's asymmetry
    double sourceOhms = 68.0e3; // Rs: impedance driving the grid (previous plate, pot)
    double gridLeakOhms = 1.0e6; // Rg
    double cathodeShelfDb = 0.0; // gain below the cathode corner relative to above it, in [−20, 0]; 0 = off
    double outputGainDb = 0.0; // after the normalised curve (f'(0) = 1)
    bool invert = true;
};

class PreampStage {
public:
    static constexpr double kDelaySamples = 1.0; // ADAA2, at the stage rate
    static constexpr int kTableSize = 1024; // intervals on [kTableMin, kTableMax]
    static constexpr double kTableMin = -4.0, kTableMax = 4.0;
    static constexpr double kAdaaTolerance = 1.0e-5;

    PreampStage() noexcept;

    // Non-RT, no allocation: solves the load line into the table (about 10^5 transcendental calls). sampleRate is
    // the rate process() runs at.
    void prepare(double sampleRate, const PreampConfig& config = {}) noexcept;
    // RT-safe. Clears every state and jumps every smoother to its target: two renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Clamped; NaN gives the range minimum (#138).
    void setDriveDb(float dB) noexcept; // −40…+40, default 0; 30 ms one-pole on the linear gain
    void setCouplingHz(float hz) noexcept; // 1…500, default 12; also sets the bias-shift release
    void setMillerHz(float hz) noexcept; // 1000…24000 (capped at 0.45·fs), default 20000
    void setCathodeHz(float hz) noexcept; // 20…2000, default 200 (used only when cathodeShelfDb < 0)

    // RT-safe, in place, at the stage rate. Before prepare() it zero-fills.
    void process(float* io, int numSamples) noexcept;

    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }
    // Group delay of the small-signal path at the current targets, in samples at the stage rate (control thread).
    double groupDelaySamples(double hz) const noexcept;
    double getMeasuredDelaySamples() const noexcept { return groupDelaySamples(1000.0); }
    int getLatencySamples() const noexcept;
    // Small-signal gain of the modelled circuit (the curve itself is normalised to slope 1).
    double circuitGainDb() const noexcept { return circuitGainDb_; }
    // Coupling-cap voltage in grid units: the grid bias is shifted by −getBiasShift() (tests, meters).
    double getBiasShift() const noexcept { return capState_; }

    // The static curve and its first and second antiderivatives (normalised; F1(0) = F2(0) = 0).
    double curve(double x) const noexcept;
    double curveIntegral(double x) const noexcept;
    double curveIntegral2(double x) const noexcept;

private:
    struct Node {
        double f = 0.0, F1 = 0.0, F2 = 0.0;
    };

    const Node& locate(double x, double& frac) const noexcept; // table interval of x, kTableMin < x < kTableMax
    void buildTable() noexcept;
    void setTargets() noexcept;

    double sampleRate_ = 0.0;
    PreampConfig config_{};
    std::array<Node, kTableSize + 1> table_{};
    double tailY_ = 0.0, tailSlope_ = 0.0, tailT_ = 1.0, lowSlope_ = 0.0, circuitGainDb_ = 0.0;
    double attackRatio_ = 1.0; // ω_a/ω_r = (Rs + Rg)/(Rs + RGI)
    double conductOffset_ = 0.0; // ω_a/(ω_r + ω_a): the conducting regime's target is x − this (onset 1)
    double shelfCut_ = 0.0; // 1 − 10^(cathodeShelfDb/20)
    double outGain_ = 1.0; // signed

    float driveDb_ = 0.0f, couplingHz_ = 12.0f, millerHz_ = 20000.0f, cathodeHz_ = 200.0f;
    OnePoleSmoother drive_; // 30 ms, linear pre-gain
    OnePoleSmoother gCoupling_, gConduct_, gMiller_, gCathode_; // 20 ms, TPT G = g/(1 + g)

    double capState_ = 0.0, millerState_ = 0.0, cathodeState_ = 0.0; // TPT integrator states
    double x1_ = 0.0, x2_ = 0.0, F2x1_ = 0.0, d12_ = 0.0; // ADAA2 history
};

} // namespace osp::dsp::amp
