#pragma once

// Measurement harness for block tests (ADR-0007 §2, REQ-TEST-002/003/004; ADR-0005 D5; oversampling.md §3.3).
// Everything is float64 and runs off the audio thread: it allocates freely. The FFT is Signalsmith dsp
// (MIT, ADR-0006 D2).

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace osp::measure {

// --- FFT -----------------------------------------------------------------------------------------------------

// Rectangular-window real FFT of x (size must be a power of two, >= 2, else std::invalid_argument). Returns bins
// b = 0 … N/2 (N/2 + 1 values), unnormalised: a sine of amplitude A on bin k gives |X[k]| = A·N/2.
std::vector<std::complex<double>> realFft(std::span<const double> x);

// |X[b]|² for b = 0 … N/2.
std::vector<double> powerSpectrum(std::span<const double> x);

// --- Coherent (bin-centred) stimulus, ADR-0007 §2 / oversampling.md §3.3 ------------------------------------------

constexpr std::size_t kCoherentFftSize = 65536; // 2^16
constexpr std::size_t kWarmupSamples = 8192;

struct CoherentTone {
    std::size_t bin = 0;    // odd, so coprime with the FFT size
    double frequency = 0.0; // exactly bin · fs / N
};

// k = round(f0·N/fs), bumped to k + 1 if even.
CoherentTone coherentTone(double nominalHz, double sampleRate, std::size_t fftSize = kCoherentFftSize);

// amplitude · sin(2π·f·n/fs) for n = 0 … length − 1 (the phase is continuous across the warm-up cut, because
// the tone completes an integer number of cycles in N samples).
std::vector<float> makeTone(const CoherentTone& tone, double sampleRate, double amplitudeDbfs, std::size_t length);

struct HarmonicReport {
    double fundamentalDbfs = 0.0;      // 20·log10(peak amplitude) of bin k
    double thd = 0.0;                  // sqrt(Σ_H |X|²) / |X[k]|  (ratio, not %)
    double thdN = 0.0;                 // sqrt(Σ_{b≠0,k} |X|²) / |X[k]|: harmonics + aliases + noise
    double asrDb = 0.0;                // 10·log10(Σ_A / Σ_H)
    double aliasToFundamentalDb = 0.0; // 10·log10(Σ_A / |X[k]|²)
    double worstAliasDbc = 0.0;        // max_{b∈A} 10·log10(|X[b]|² / |X[k]|²)
    bool harmonicsBelowFloor = false;  // H empty (f0 > fs/4) or Σ_H < −150 dBFS: gate on alias-to-fundamental
};

// Analyses exactly the last `fftSize` samples of `output` (the warm-up before them is ignored). Bin sets as
// oversampling.md §3.3 step 3: H = {m·k : m ≥ 2, m·k < N/2}, A = [1, N/2) \ ({k} ∪ H). DC and Nyquist excluded.
HarmonicReport analyseCoherent(std::span<const float> output, std::size_t bin, std::size_t fftSize = kCoherentFftSize);

// --- Latency-aligned null, REQ-TEST-004 / REQ-TEST-005 tier 1 -----------------------------------------------------

struct NullResult {
    double rmsDbfs = 0.0;  // 20·log10(RMS of the residual); −inf for a perfect null
    double peakDbfs = 0.0; // 20·log10(max |residual|)
    std::size_t compared = 0;
};

// Residual r[i] = output[i + latency] − reference[i] over every i where both exist.
NullResult nullTest(std::span<const float> output, std::span<const float> reference, std::size_t latency = 0);

// --- Parameter smoothing, REQ-TEST-003 (as amended by PR #81 / REQ-423) -------------------------------------------

// THE GATE: RMS of r[i] = output[i + latency] − reference[i] over the overlap, relative to the steady-state RMS of
// the reference at f0, in dBc. Steady state = the louder of the first and last 10 % of the overlap, so put the
// step in the middle and let the law settle before the last 10 %. The reference is an offline float64 render that
// applies the specified smoothing law per sample; the test passes at ≤ −60 dBc, with the parameter step on a
// non-block-aligned sample.
double residualDbc(std::span<const float> output, std::span<const double> reference, std::size_t latency = 0);
double residualDbc(std::span<const float> output, std::span<const float> reference, std::size_t latency = 0);

// INFORMATIVE ONLY (not a gate): energy outside [f0 − guardHz, f0 + guardHz] relative to the energy inside, in
// dBc, over the whole of `output` (periodic Hann window; the length must be a power of two, ≥ 16384 so the
// window's own leakage stays out). An ideal linear 50 ms +6 dB ramp already measures about −56 dBc.
double splatterDbc(std::span<const float> output, double sampleRate, double f0, double guardHz = 50.0);

double toDb(double ratio) noexcept;        // 20·log10, −inf at 0
double powerToDb(double power) noexcept;   // 10·log10, −inf at 0

} // namespace osp::measure
