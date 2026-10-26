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

#include <vector>

namespace osp::dsp {

struct IrLoader {
    static constexpr double kMaxIrSeconds = 1.0;
    static constexpr double kCapFadeSeconds = 0.010;
    static constexpr double kCutoff = 0.45;
    static constexpr int kZeroCrossings = 32;
    static constexpr double kKaiserBeta = 9.0;

    // Resamples, caps and sanitises. irRate <= 0 is taken as processRate.
    static std::vector<float> condition(const float* ir, int length, double irRate, double processRate);
    // Band-limited resampling only (an exact copy when the rates are equal).
    static std::vector<float> resample(const float* ir, int length, double fromRate, double toRate);
};

} // namespace osp::dsp
