#pragma once

// Measurement harness, part 2 (ADR-0007 §2 and §3): exponential sine sweep with harmonic separation, and the
// tier-2 spectral distances for golden renders. float64, off the audio thread.

#include <cstddef>
#include <span>
#include <vector>

namespace osp::measure {

// --- Exponential sine sweep, REQ-TEST-002 --------------------------------------------------------------------------
// Synchronized swept-sine (Novak, Lotton & Simon, "Synchronized Swept-Sine: Theory, Application, and
// Implementation", JAES 63(10), 2015), after Farina (AES 108th Convention, preprint 5093, 2000):
// x(t) = A·sin(2π·f1·L·e^(t/L)), with L = round(f1·T / ln(f2/f1)) / f1. The m-th harmonic of the output then
// equals the sweep advanced by L·ln(m), so deconvolution puts its impulse response at t = −L·ln(m).

struct EssConfig {
    double sampleRate = 48000.0;
    double f1 = 20.0;
    double f2 = 20000.0;
    double seconds = 2.0; // nominal; L is rounded so f1·L is an integer
    double amplitude = 0.5;
    std::size_t tailSamples = 16384; // silence after the sweep, for the system's decay
};

// The sweep followed by `tailSamples` zeros. Render it through the block and pass the output to analyseEss.
std::vector<float> makeEss(const EssConfig& config);

class EssResponse {
public:
    // |H_m| in dB at excitation frequency f: the m-th harmonic's output lies at m·f, which must be below fs/2.
    // Order 1 is the linear frequency response (0 dB for a wire). Orders 2…5 are the harmonic responses,
    // relative to the sweep amplitude, so H_m/H_1 at f is the m-th harmonic distortion of a tone at f.
    // Throws std::invalid_argument when the order is out of range or m·f ≥ fs/2.
    double gainDb(int order, double excitationHz) const;
    int maxOrder() const noexcept { return static_cast<int>(magnitude_.size()); }

private:
    friend EssResponse analyseEss(std::span<const float>, const EssConfig&, int);
    double binHz_ = 0.0;
    std::vector<std::vector<double>> magnitude_; // [order − 1][bin]
};

// Deconvolves `output` (the response to makeEss(config), at least that long) with a regularised inverse of the
// sweep, then windows each harmonic impulse response and transforms it. The H1 window is ≈ L·ln 2 (8192 samples
// at the defaults, 0.17 s at 48 kHz), so H1 is valid for blocks whose impulse response fits in it. For longer
// tails (reverb, long delays), raise `seconds`. Invalid input throws std::invalid_argument.
EssResponse analyseEss(std::span<const float> output, const EssConfig& config, int maxOrder = 5);

// --- Tier-2 spectral distance, REQ-TEST-005 --------------------------------------------------------------------

struct SpectralDistance {
    double meanLsdDb = 0.0;           // LSD at N = 2048, averaged over frames (gate: ≤ 0.2 dB)
    double maxThirdOctaveLsdDb = 0.0; // worst 1/3-octave band, RMS over frames of the band-level difference (≤ 1 dB)
    double spectralConvergence = 0.0; // MR-STFT, mean over N ∈ {512, 1024, 2048} of ‖|R|−|T|‖F / ‖|R|‖F (≤ 1e-3)
};

// STFT with a periodic Hann window and hop N/4 over the common length. Power is normalised so a full-scale sine
// reads 1 (0 dB) in its bin and is clamped at 1e-10 (−100 dBFS) before taking logs. Fewer than 2048 common samples
// throws std::invalid_argument, so a short or truncated render cannot pass vacuously.
SpectralDistance spectralDistance(std::span<const float> test, std::span<const float> reference, double sampleRate);

} // namespace osp::measure
