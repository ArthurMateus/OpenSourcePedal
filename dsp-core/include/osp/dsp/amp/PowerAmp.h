#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Classic-amp power stage (ADR-0011 D3/D7 slice P5-7, issue #173): push-pull shaper, global negative feedback (NFB)
// with presence and depth inside the loop (parameters.md §2.5), supply sag, `master` drive. It runs at the rate passed
// to prepare(), which is the OS-2 rate inside the classic amp (ADR-0005 D2: 176.4…768 kHz). Design notes:
// docs/research/classic-amp-models.md §3.2–§3.4, §4 (Q6–Q8).
//
// Signal flow, with N = 10^(nfbDb/20) the midband loop gain and L the (sagged) ceiling:
//   u = G_master·(1 + N)/N · x                 (so the small-signal midband gain is G_master)
//   φ = L·f(w/L),  w = N·(u − B(φ))            B = HPF_loop · S_presence · S_depth, unity at mid
//   y = HPF_loop(ADAA1 of the loop map)        (output transformer primary pole, inside the loop)
//   out = post HPF/LPF(y)                      (transformer/voicing band limits, outside the loop)
// f is odd: tanh (beam-tetrode knee) or x/(1+|x|) (pentode-like soft knee).
//
// Delay-free loop (research Q6). B's direct term B0 is split off: B(φ) = B0·φ + c0, c0 from the stored filter states.
// In units of L, with γ = N·(u − c0)/L and K = N·B0, the loop is the memoryless map  φ = Φ(γ),  ω + K·f(ω) = γ,
// Φ = f(ω). Rational shape: ω is the root of a quadratic (closed form). tanh: ψ(ω) = ω + K·tanh ω − |γ| is concave
// and increasing on ω ≥ 0, so Newton from max(|γ|/(1 + K), |γ| − K), a point left of the root, rises monotonically
// to it (at most maxNewtonIterations steps, bounded work, RT-7). The loop filters run on Φ, so the discrete loop is
// exactly the bilinear transform of the modelled one, T = N·B, with no added delay.
// Antialiasing: ADAA1 of the map Φ gives the output. By substitution (dγ = (1 + K·f'(ω))dω) its antiderivative is
// closed form, G(γ) = F(ω) + K·f(ω)²/2, so v = L·(G(γ) − G(γ1))/(γ − γ1) (midpoint mean below |Δγ| = 1e-5); the
// output tap has its own copy of HPF_loop. This is our own derivation. Alternatives prototyped and dropped:
// - ADAA of f inside the implicit equation (research §3.3 a) linearises to 1 + K·(1 + z⁻¹)/2 = 0, a pole at
//   −K/(2 + K) (−0.94 at the largest loop gain): the internal state alternates at Nyquist and the trajectory becomes
//   sensitive to rounding. Feeding the ADAA output back through the states instead left a gain margin of 3.6 dB.
// - A one-sample delay in β (research §3.3 b): its phase margin is negative (the tests print it), i.e. unstable.
// When L or K change, G(γ1) is re-expressed to first order (dG/dγ = Φ, dG/dK = −Φ²/2): the history is re-evaluated
// with the current parameters (research §4) without a second solve.
//
// Presence/depth (parameters.md §2.5): the small-signal closed loop is designed to be a 1st-order shelf of gain
// (knob − 5)·2 dB (±10 dB at 0/10), midpoint sqrt(2000·6000) Hz (presence) or sqrt(60·150) Hz (depth), so at ±10 dB
// its zero and pole sit at ~2/6 kHz and ~53/169 Hz. For that target C(s), the β section is S = ((1 + N)/C − 1)/N, also
// 1st order and minimum phase while the gain stays below 1 + N. Bilinear transform prewarped at the midpoint. Because
// the shelves sit in the loop, their effect fades as the stage saturates and the loop gain collapses (intended).
//
// Supply sag (research Q8). A plain physical model of a rectifier + reservoir-capacitor supply: the push-pull stage
// draws a supply current proportional to |v|/L0 (class-B-like conduction); the rail is the state of a one-pole RC
// network that droops toward 1 − S·i while the load exceeds the charge (attack time constant: reservoir discharge)
// and recharges through the rectifier's source resistance (release time constant); the ceiling is L = L0·rail.
// S is the fractional droop at full continuous load. The rail moves after each sample and acts on the next one.
// Patent check: not a legal review; generic RC supply model. The research cites a patent on envelope-driven sag from a
// search listing only; its claims were not read. Because of that open item, sagAmount defaults to 0 here, and a
// voicing (#174) should not ship a non-zero value until the owner has reviewed it.
//
// Deferred to v2 (research Q7): transformer LF saturation, speaker-impedance interaction, class-AB crossover. The OT
// leakage pole is outside the loop (post LPF), so the loop holds one HPF pole and the two shelves only.
//
// Latency: kDelaySamples = 0.5 sample at the stage rate (ADAA1); everything else is minimum phase. At the OS-2 rate
// that is 2.8–0.65 µs, so getLatencySamples() (integer samples) is 0; the classic amp measures its total (ADR-0011 D2).
//
// Sources: Parker, Zavalishin, Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time
// Convolution", DAFx-16 (ADAA1); V. Zavalishin, "The Art of VA Filter Design" rev. 2.1.2 (2018), ch. 3 and 5
// (instantaneous-response splitting of zero-delay feedback loops); supply physics:
// https://www.ampbooks.com/mobile/dsp/power-supply/ (accessed 2026-10-28). Written from these equations; no code
// copied.

#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"
#include "osp/rt/AudioThread.h"

#include <complex>

namespace osp::dsp::amp {

enum class PowerAmpShape { beam, pentode }; // tanh, x/(1+|x|)

// Per-model constants (#174 picks them per voicing). Engineering starting points, unverified by listening.
struct PowerAmpVoicing {
    PowerAmpShape shape = PowerAmpShape::pentode;
    double nfbDb = 14.0; // midband loop gain, clamped to [6, 20]
    double ceiling = 1.0; // output ceiling L0 at full supply
    double masterDbAt5 = 0.0; // drive into the stage at master 5
    double masterDbPerKnob = 3.0; // master law: masterDbAt5 + (knob − 5)·this
    double loopHpHz = 4.0; // transformer primary pole, inside the loop
    double otHpHz = 50.0; // 2nd-order HPF outside the loop; 0 = off
    double otLpHz = 11000.0; // 2nd-order LPF outside the loop; 0 = off
    double otLpQ = 0.8;
    double sagAmount = 0.0; // S, clamped to [0, 0.5]; 0 by default, see the patent note above
    double sagAttackMs = 20.0;
    double sagReleaseMs = 200.0;
    int maxNewtonIterations = 8; // tanh only; clamped to [1, 64]
};

class PowerAmp {
public:
    static constexpr double kPresenceHz = 3464.1016151377544; // sqrt(2000·6000)
    static constexpr double kDepthHz = 94.868329805051380; // sqrt(60·150)

    PowerAmp() noexcept; // knobs at their parameters.md defaults (5)

    // Non-RT. Takes effect at the next prepare().
    void setVoicing(const PowerAmpVoicing& voicing) noexcept { voicing_ = voicing; }
    // Non-RT, no allocation. sampleRate is the rate the stage runs at (the OS-2 rate in a classic amp).
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears every state and jumps the smoothers to their targets: two renders after reset() match.
    OSP_RT_NONBLOCKING void reset() noexcept;

    // RT-safe, once per block. Knobs 0…10, clamped; NaN gives 0. Smoothed per sample (20/20/30 ms, parameters.md).
    OSP_RT_NONBLOCKING void setPresence(float knob) noexcept;
    OSP_RT_NONBLOCKING void setDepth(float knob) noexcept;
    OSP_RT_NONBLOCKING void setMaster(float knob) noexcept;

    // RT-safe, in place. Before prepare() it zero-fills. Non-finite input samples are treated as 0.
    OSP_RT_NONBLOCKING void process(float* io, int numSamples) noexcept;

    static constexpr double kDelaySamples = 0.5; // ADAA1, at the stage rate
    int getLatencySamples() const noexcept { return 0; }
    double getSupplyDroop() const noexcept { return droop_; } // 1 − rail, 0…sagAmount
    bool isPrepared() const noexcept { return fs_ > 0.0; }

    // Non-RT analysis at the knob targets, linearised at rest (f'(0) = 1, so ADAA1 is h = (1 + z⁻¹)/2).
    // loopGain: the loop T = N·B that the delay-free solve closes; with unitDelay, the one-sample-delay
    // alternative T·z⁻¹. response: input to output, post filters included.
    std::complex<double> loopGain(double hz, bool unitDelay = false) const noexcept;
    std::complex<double> response(double hz) const noexcept;

private:
    struct Section { // 1st-order TDF-II section, double state (it sits in the loop)
        double b0 = 1.0, b1 = 0.0, a1 = 0.0, s = 0.0;
        double tick(double x) noexcept;
        std::complex<double> at(std::complex<double> zInv) const noexcept;
    };
    static Section shelf(bool high, double knob, double n, double warp) noexcept;
    void updateShelf(Section& section, bool high, float knob) noexcept;
    void updateMaster(float knob) noexcept;
    void setCeiling(double l) noexcept;
    void loopMap(double gamma, double k, double& phi, double& g) const noexcept; // Φ(γ) and G(γ)

    PowerAmpVoicing voicing_{};
    double fs_ = 0.0;
    double n_ = 5.0, l0_ = 1.0, sag_ = 0.0, sagAttack_ = 0.0, sagRelease_ = 0.0, warpP_ = 1.0, warpD_ = 1.0;
    int iterations_ = 8;
    bool beam_ = false;
    Section hp_, hpOut_, presence_, depth_;
    double gIn_ = 1.0; // G_master·(1 + N)/N
    double l_ = 1.0; // current ceiling
    double gamma1_ = 0.0, g1_ = 0.0, phi1_ = 0.0, k1_ = 0.0; // ADAA history: γ, G, Φ at n − 1, and the K of G
    double droop_ = 0.0;
    SvfCoeffs postHpC_{}, postLpC_{};
    SvfState postHp_, postLp_;
    bool postHpOn_ = false, postLpOn_ = false;
    OnePoleSmoother presenceK_, depthK_, masterK_;
};

} // namespace osp::dsp::amp
