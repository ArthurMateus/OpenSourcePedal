#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Worker-side IR conditioning for the cab slots (parameters.md §3.1, ADR-0004 §4): the IR file's samples are
// resampled to the processing rate and capped in length, then handed to PartitionedConvolver::prepare(). Non-RT:
// everything here allocates.
//
// Resampling: band-limited interpolation with a Kaiser-windowed sinc, J. O. Smith, "Digital Audio Resampling Home
// Page", https://ccrma.stanford.edu/~jos/resample/ (accessed 2026-10-26); Kaiser window and its I0 series from
// J. F. Kaiser, "Nonrecursive digital filter design using the I0-sinh window function", Proc. IEEE ISCAS 1974. The
// cutoff is kCutoff·min(rates) with kZeroCrossings sinc lobes per side and β = kKaiserBeta (about −90 dB stopband),
// so the response is flat to about 0.41·min(rates) (19.6 kHz at 48 kHz). The taps are scaled by irRate/processRate
// so the frequency response, not the tap sum, is preserved. The kernel is symmetric (linear phase, no added delay,
// L_cab = 0); only the band-limited pre-ringing that would fall before the IR's first sample is dropped.
// besselI0 runs per tap (~0.1-0.3 s per 1 s IR on the worker, unverified); a window table can come in a perf pass.
//
// Length cap (own decision, #69): kMaxIrSeconds at the processing rate, the budget case of the cab block (2 × 1 s at
// 48 kHz / 64). A longer IR is truncated with a raised-cosine fade over its last kCapFadeSeconds, so the cut adds no
// click to the tail. Cab IRs are typically far shorter; the bundled 4×12 set is 1.2–1.3 s only because of its
// decoded noise tail. Non-finite input samples become 0.
//
// Level (ADR-0010, #158): by default the conditioned IR is scaled so that its power gain for pink noise between
// kRefLowHz and kRefHighHz is 0 dB: g = 1/sqrt(Σ|H(f_k)|²/f_k / Σ1/f_k) over the FFT bins of that band (pink noise
// has a 1/f power density, so the 1/f weights make the ratio the pink-noise power gain out/in). Cab IRs are usually
// peak-normalised (the bundled set carries +12.3…+17.8 dB of pink-band gain), so without this, enabling the cab or
// swapping IRs jumps the level. A unit impulse stays bit-exact (both sums are built from identical terms), a gain
// within kDeadbandDb of 0 dB is not applied (normalising twice changes nothing), and the correction is clamped to
// ±kMaxCorrectionDb so that a near-silent file is not lifted into noise; an all-zero IR is returned unchanged.
// IrLevel::Keep skips the step: the IR keeps the file's own level.

#include <vector>

namespace osp::dsp {

enum class IrLevel {
    Normalise, // pink-band power gain 0 dB (the default, for factory and user IRs alike)
    Keep,      // the file's own level
};

struct IrLoader {
    static constexpr double kMaxIrSeconds = 1.0;
    static constexpr double kCapFadeSeconds = 0.010;
    static constexpr double kCutoff = 0.45;
    static constexpr int kZeroCrossings = 32;
    static constexpr double kKaiserBeta = 9.0;
    static constexpr double kRefLowHz = 40.0;
    static constexpr double kRefHighHz = 20000.0;
    static constexpr double kDeadbandDb = 0.01;
    static constexpr double kMaxCorrectionDb = 40.0;

    // Resamples, caps, sanitises and (by default) normalises the level. irRate <= 0 is taken as processRate.
    static std::vector<float>
    condition(const float* ir, int length, double irRate, double processRate, IrLevel level = IrLevel::Normalise);
    // Power gain in dB of `ir` (at `rate`) for pink noise band-limited to kRefLowHz…min(kRefHighHz, 0.45·rate).
    // -inf for an empty or all-zero IR.
    static double pinkBandGainDb(const float* ir, int length, double rate);
    // Scales `ir` in place to a 0 dB pink-band gain, with the deadband and clamp above. Returns the applied dB.
    static double normalise(float* ir, int length, double rate);
    // Band-limited resampling only (an exact copy when the rates are equal).
    static std::vector<float> resample(const float* ir, int length, double fromRate, double toRate);
};

} // namespace osp::dsp
