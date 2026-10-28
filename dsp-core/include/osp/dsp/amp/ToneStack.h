#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Amp tone stack (ADR-0011 D7 slice P5-6, #172): the classic passive treble/bass/middle network, mono, run at the rate
// given to prepare() (inside the amp's OS-2 region, 176.4...768 kHz; the base rates work too). Latency 0.
//
// Network (generic names; values per variant below):
//   Vin -Rs- V0                                 Rs = output impedance of the driving stage
//   V0 -C1- V1,  V1 -(1-t)R1- Vo -tR1- V2       treble pot as a divider, wiper = output Vo
//   V0 -R4- V3 (slope),  V3 -C2- V2,  V3 -C3- V4
//   V2 -lR2- V4 (bass rheostat),  V4 -mR3- gnd (middle rheostat),  Vo -RL- gnd (load: the next volume pot / grid)
// H(s) = (b1 s + b2 s^2 + b3 s^3) / (a0 + a1 s + a2 s^2 + a3 s^3), closed forms in the pot fractions t, l, m from our
// own nodal analysis (tools/amp/tonestack_derive.py, sympy; matches a direct complex solve to 1e-14). b0 = 0: the
// stack blocks DC. Method idea only (symbolic analysis of the stack, then a per-block discretisation): D. T. Yeh and
// J. O. Smith, DAFx-06, https://dafx.de/paper-archive/details/bbvGpyU59H2nHvV3Iamezg (accessed 2026-10-28). No
// formula, coefficient or code was taken from that paper, its web page or any other implementation.
//
// Discretisation (decision, measured in #172): NOT the bilinear transform. With treble and middle low the network
// still falls 6 dB/oct above 16 kHz, and the bilinear map folds s = inf onto Nyquist: 5.6 dB error at 16 kHz with
// fs = 44.1 kHz and 0.2 dB even at 192 kHz; prewarping cannot fix an asymptote. Instead, per coefficient update:
//  1. Poles. An RC network has real negative poles only. The root nearest 0 comes from Newton's method started at
//     s = 0 (monotone for a real-rooted polynomial), the other two from the deflated quadratic. They are mapped
//     exactly, z = exp(pT) (matched z-transform, https://en.wikipedia.org/wiki/Matched_Z-transform_method, accessed
//     2026-10-28).
//  2. The DC zero is kept exactly as the factor (1 - z^-1).
//  3. The rest of the numerator, B'(z) = c0 + c1 z^-1 + c2 z^-2, is matched in magnitude: |B'|^2 is a quadratic
//     P(y) in y = 1 - cos(w), fitted by weighted least squares (Householder QR) to |H(jW) A(z) / (1 - z^-1)|^2 at
//     kFitPoints frequencies (W is the true analog frequency, so there is no warping). kLawsonPasses reweighting passes
//     (Lawson's iteration) push the fit towards minimax. Then P is factored in closed form into its minimum-phase
//     B' (Fejer-Riesz); the analog zeros are in the left half-plane on every setting we checked, so the phase matches
//     as well. Fit band 20 Hz...16 kHz below 80 kHz and 20 Hz...20 kHz from 88.2 kHz up. Measured: <= 0.08 dB from
//     the analog response over 20 Hz...16 kHz at 44.1/48 kHz and <= 0.01 dB over 20 Hz...20 kHz at >= 88.2 kHz.
//     Above the fit band (ultrasonic, inside OS-2) the error can reach about 1.3 dB.
// Realisation, all in double: the modal (parallel) form y[n] = d x[n] + sum_i r_i w_i[n], w_i[n] = x[n-1] + z_i
// w_i[n-1], with the residues r_i of the design. Each mode is stable for any coefficient sequence with 0 <= z_i < 1, so
// ramping d, r_i and z_i per sample cannot destabilise it. Measured in #172: shifting a knob sweep's coefficient
// trajectory by one sample changes the output by -71 dBc in this form and by -37 dBc in the direct cascade (1 - z^-1)
// B'(z) / A(z), whose taps cancel at low frequencies, so the modal form is the one that does not zipper.
//
// Smoothing (parameters.md §2.5): bass, mid and treble (0...10) are one-pole 20 ms. While one moves, the stack is
// designed at the end of every chunk, from the smoothers' values there, and d, r_i and z_i ramp linearly per sample
// across it, so the trajectory follows the per-sample law to 2nd order. A chunk ends at the block end, after
// kUpdateSeconds, or earlier: it is halved until no pot's log-resistance moves by more than kMaxLogStep across it (the
// poles move roughly as 1/R near a pot's end). A static setting costs no design work. parameters.md §1.4 asks for a
// tone-knob update every sample; a full design costs ~1.6 us, so this tracks it instead and #172's REQ-TEST-003 test
// measures the difference against a per-sample design (<= -90 dBc).
// Pot laws: audio taper R(x) = (81^x - 1)/80 (10 % of the track at mid-travel), linear R(x) = x. A rheostat has an end
// resistance of kMinRheostat of its track in series, l = kMin + (1 - kMin) R(x): a real pot has one, it keeps a(s) at
// 3rd order, and unlike a max() floor it has no kink (a kink broke the coefficient interpolation at bass 0 -> 10).

#include "osp/dsp/util/Smoothers.h"
#include "osp/rt/AudioThread.h"

#include <array>

namespace osp::dsp::amp {

// Component values (ohms, farads). The variants are generic textbook values, *unverified* against a primary source
// (#172 PR): research note classic-amp-models.md §3.1/§5 and recollection.
struct ToneStackComponents {
    double rSource, rTreble, rBass, rMid, rSlope, rLoad; // Rs, R1, R2, R3, R4, RL
    double cTreble, cBass, cMid; // C1, C2, C3
    bool trebleAudio, bassAudio, midAudio; // pot laws: true = audio taper, false = linear
};

// The variants. Values: C1 / C2 / C3, R1 (treble) / R2 (bass) / R3 (mid), R4 (slope), Rs, RL.
enum class ToneStackVariant {
    a, // US-style, plate-driven (osp.amp.clean_a): 250 pF / 100 nF / 47 nF, 250 k / 250 k / 10 k, 100 k, 38 k, 1 M
    b, // UK-style, cathode follower (crunch_a, lead_a): 470 pF / 22 nF / 22 nF, 220 k / 1 M / 25 k, 33 k, 1 k, 1 M
    c, // early wide-range, cathode follower: 250 pF / 20 nF / 20 nF, 250 k / 1 M / 25 k, 56 k, 1 k, 1 M
};

// Rs: a 100 k plate load in parallel with about 62 k triode plate resistance (a), or a cathode follower's ~1 k (b, c).
// RL: a 1 M volume pot. Bass pot audio taper, treble and mid linear. All *unverified*.
// clang-format off
//                                                Rs    R1     R2     R3    R4     RL   C1       C2      C3
inline constexpr ToneStackComponents kToneStackA{38e3, 250e3, 250e3, 10e3, 100e3, 1e6, 250e-12, 100e-9, 47e-9,
                                                 false, true, false}; // audio taper: treble, bass, mid
inline constexpr ToneStackComponents kToneStackB{1e3,  220e3, 1e6,   25e3, 33e3,  1e6, 470e-12, 22e-9,  22e-9,
                                                 false, true, false};
inline constexpr ToneStackComponents kToneStackC{1e3,  250e3, 1e6,   25e3, 56e3,  1e6, 250e-12, 20e-9,  20e-9,
                                                 false, true, false};
// clang-format on
const ToneStackComponents& toneStackComponents(ToneStackVariant variant) noexcept;

// s-domain coefficients, index = power of s (b[0] is always 0).
struct ToneStackAnalog {
    std::array<double, 4> b{}, a{};
};

// The digital stack H(z) = (1 - z^-1)(c0 + c1 z^-1 + c2 z^-2) / prod_i (1 - z_i z^-1), in modal form:
// H(z) = d + sum_i r_i z^-1 / (1 - z_i z^-1), z[0] >= z[1] >= z[2].
struct ToneStackCoeffs {
    double d = 0.0;
    std::array<double, 3> r{}, z{};
};

inline constexpr double kMinRheostat = 1e-3;

// Knob 0...10 -> pot fraction 0...1 after the taper (NaN -> 0, clamped). A rheostat adds kMinRheostat of end
// resistance.
double toneStackPotFraction(double knob, bool audioTaper, bool rheostat) noexcept;
// Closed-form analog coefficients for pot fractions t (treble), l (bass), m (middle).
ToneStackAnalog toneStackAnalog(const ToneStackComponents& k, double t, double l, double m) noexcept;

// The per-update design (steps 1-3 above). prepare() fixes the fit frequencies for a rate; design() is RT-safe and
// allocation-free, about 7k flops. It returns false (and leaves `out` alone) if the fit is degenerate.
class ToneStackDesigner {
public:
    static constexpr int kFitPoints = 26; // 10 log-spaced up to top/2, 16 linear from top/2 to top
    static constexpr int kLawsonPasses = 4;

    void prepare(double sampleRate) noexcept;
    bool design(const ToneStackComponents& k,
                double bass,
                double mid,
                double treble,
                ToneStackCoeffs& out) const noexcept OSP_RT_NONBLOCKING;
    double sampleRate() const noexcept { return fs_; }

private:
    double fs_ = 0.0;
    std::array<double, kFitPoints> omega_{}, y_{}; // analog rad/s, and 1 - cos(w) = 2 sin^2(w/2)
};

class ToneStack {
public:
    static constexpr double kSmoothingSeconds = 0.020;
    static constexpr double kUpdateSeconds = 1.0 / 1500.0; // design at least every 0.67 ms while a knob moves

    ToneStack() noexcept;

    // Non-RT (no allocation either). Clears the state, the smoothers jump to their targets.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears the filter state, the smoothers jump to their targets: two renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Knobs 0...10 (parameters.md §2.5); NaN maps to 0, values are clamped.
    void setBass(float knob) noexcept { bass_.setTarget(clampKnob(knob)); }
    void setMid(float knob) noexcept { mid_.setTarget(clampKnob(knob)); }
    void setTreble(float knob) noexcept { treble_.setTarget(clampKnob(knob)); }
    // RT-safe. A voicing choice: the next update ramps to the new network over one chunk (no crossfade).
    void setVariant(ToneStackVariant variant) noexcept;
    void setComponents(const ToneStackComponents& components) noexcept;

    // RT-safe. In place, mono. Before prepare() it zero-fills. Non-finite input clears the state at the block end.
    void process(float* io, int numSamples) noexcept OSP_RT_NONBLOCKING;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return designer_.sampleRate() > 0.0; }
    // For tests: the coefficients in use, and how many designs failed (kept the previous coefficients).
    const ToneStackCoeffs& coefficients() const noexcept { return cur_; }
    int designFailures() const noexcept { return failures_; }

private:
    static float clampKnob(float knob) noexcept { return knob >= 0.0f ? (knob <= 10.0f ? knob : 10.0f) : 0.0f; }
    static constexpr double kMaxLogStep = 0.1; // largest log-resistance change of any pot across one chunk

    template <bool Ramp>
    void run(float* io, int n) noexcept;
    std::array<double, 4> logPots(double bass, double mid, double treble) const noexcept;
    int limitChunk(int len) const noexcept;
    void clearState() noexcept;

    ToneStackDesigner designer_;
    ToneStackComponents components_;
    OnePoleSmoother bass_, mid_, treble_;
    ToneStackCoeffs cur_, target_, step_;
    int interval_ = 1;
    int failures_ = 0;
    bool jump_ = true; // the next design is applied without a ramp (after prepare/reset)
    bool dirty_ = true; // components changed: design even if no knob moves
    double x1_ = 0.0; // previous input
    std::array<double, 3> w_{}; // modal states
};

} // namespace osp::dsp::amp
