#pragma once

// Tone profile (ADR-0008 D2): what the tone matcher compares. Offline and non-RT: everything here allocates.
// - pickSource: the most guitar-heavy mono signal of a (stereo) reference mix.
// - analyse: median-filter HPSS drops percussive energy (D. FitzGerald, "Harmonic/percussive separation using
//   median filtering", DAFx-10, 2010), then a long-term average spectrum (LTAS) on a 1/4-octave grid and the
//   spectral flatness per octave (500 Hz…8 kHz) of each frame divided by the LTAS. Flatness rises as clipping fills
//   the gaps between harmonics, and a static filter (EQ, cab) scales frame and LTAS alike, so it measures "how much
//   gain" independently of the tone shaping.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace osp::engine::tonematch {

// 1/4-octave analysis grid, 50 Hz … 16 kHz.
constexpr int kBands = 34;
constexpr double kBandBaseHz = 50.0;
constexpr int kBandsPerOctave = 4;
double bandHz(int band) noexcept;

// Octave bands for the flatness feature: 500-1k, 1k-2k, 2k-4k, 4k-8k.
constexpr int kFlatnessBands = 4;

enum class Source { automatic, mono, mid, side, left, right };
const char* toString(Source s) noexcept;

// The mono signal to analyse from a (possibly stereo) reference. right == nullptr means mono. automatic takes the
// side channel when, in the kSideBandHz guitar range, it holds at least kSideShare of the mid energy: hard-panned
// double-tracked guitars live there, while kick, bass and lead vocal sit in the centre and cancel.
constexpr double kSideShare = 0.15;
constexpr std::array<double, 2> kSideBandHz{200.0, 5000.0};
std::vector<float>
pickSource(const float* left, const float* right, std::size_t n, double sampleRate, Source wanted, Source* used);

struct Profile {
    std::array<double, kBands> levelDb{};          // LTAS, mean over the fit weights = 0 dB
    std::array<double, kFlatnessBands> flatnessDb{}; // Wiener entropy per octave, <= 0 dB (0 = white noise)
    double activeSeconds = 0.0;                      // frames above the silence gate
    double sampleRate = 0.0;                         // rate it was analysed at
};

struct AnalysisOptions {
    bool hpss = true;           // drop percussive energy first
    double maxSeconds = 45.0; // analyse at most this much (from the start of `x`); trim the input to choose which part
    double gateDb = -45.0; // frames this far below the loud (95th percentile) frames are ignored
};

Profile analyse(const std::vector<float>& x, double sampleRate, const AnalysisOptions& options = {});

// Band levels (dB, not normalised) of a power spectrum with bins spaced binHz apart, triangular 1/4-octave weights.
std::array<double, kBands> bandLevelsDb(const std::vector<double>& power, double binHz);
// Band response (dB) of an impulse response.
std::array<double, kBands> impulseBandsDb(const std::vector<float>& ir, double sampleRate);
// Fit weight per band: 1 in 120 Hz…6 kHz, 0.5 at 80…120 Hz and 6…10 kHz, else 0 (bass and cymbal leakage).
double fitWeight(int band) noexcept;

using Bands = std::array<double, kBands>;
// Weighted mean of x under fitWeight(), and x minus it.
double weightedMean(const Bands& x) noexcept;
Bands normalised(Bands x) noexcept;
// Weighted RMS dB of a - b after removing the weighted mean difference (levels compared up to a gain).
double residualDb(const Bands& a, const Bands& b) noexcept;
// RMS dB of the flatness difference over the flatness bands.
double flatnessDistance(const Profile& a, const Profile& b) noexcept;

} // namespace osp::engine::tonematch
