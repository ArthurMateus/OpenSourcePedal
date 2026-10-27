#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Pitch detection for the chromatic tuner, REQ-419: +-1 cent for 40-1000 Hz sines, stable on plucked guitar notes.
// It runs on a worker thread (threading.md: tuner frames are a telemetry stream), on the latest window that
// TunerTap decimated on the audio thread. prepare() is the only call that allocates; detect() is allocation-free and
// bounded (O(window x maxLag), about 0.8 M multiply-adds per estimate at the ~24 kHz tap rate).
//
// Algorithm: YIN, A. de Cheveigné and H. Kawahara, "YIN, a fundamental frequency estimator for speech and music",
// J. Acoust. Soc. Am. 111(4), 2002: difference function d(tau) (eq. 6), cumulative mean normalised difference d'(tau)
// (eq. 8), absolute threshold (step 4) and parabolic interpolation (step 5). The interpolation runs on d(tau), whose
// minimum is close to a parabola for a periodic input, not on d'(tau), whose normalisation tilts it. Own choices
// (unverified against other tuners): threshold kThreshold, level gate kGateDbfs, and the window = max lag, so the
// lowest note (kMinHz) still gets one full period of integration.

#include <vector>

namespace osp::dsp {

struct TunerReading {
    bool valid = false;      // false: too quiet, or no periodicity below the threshold
    double hz = 0.0;         // fundamental frequency
    int midiNote = 0;        // nearest equal-tempered note, 69 = A4
    double cents = 0.0;      // offset from that note, -50 ... +50
    double confidence = 0.0; // 1 - d'(tau) at the estimate, 0 ... 1
};

class PitchDetector {
public:
    static constexpr double kMinHz = 38.0;  // below the 40 Hz spec floor, so 40 Hz is not at the lag edge
    static constexpr double kMaxHz = 1100.0;
    static constexpr double kThreshold = 0.15;
    static constexpr double kGateDbfs = -60.0; // window RMS below this reads as silence
    static constexpr double kMinReferenceHz = 415.0, kMaxReferenceHz = 466.0;

    // Non-RT: allocates. sampleRate is the rate of the samples given to detect() (the tap's decimated rate).
    void prepare(double sampleRate);
    // Samples detect() needs: the window plus the largest lag.
    int requiredSamples() const noexcept { return window_ + maxLag_ + 1; }

    // tuner.referenceHz (parameters.md §4), clamped to 415...466; NaN gives 440.
    void setReferenceHz(double hz) noexcept;
    double referenceHz() const noexcept { return referenceHz_; }

    // Allocation-free. Uses the last requiredSamples() of x[0..n); fewer gives an invalid reading.
    TunerReading detect(const float* x, int n) noexcept;

    // Nearest note and cents for a frequency against a reference A4.
    static TunerReading toNote(double hz, double referenceHz) noexcept;

private:
    double sampleRate_ = 0.0;
    double referenceHz_ = 440.0;
    int minLag_ = 0, maxLag_ = 0, window_ = 0;
    std::vector<double> diff_, cmnd_;
};

} // namespace osp::dsp
