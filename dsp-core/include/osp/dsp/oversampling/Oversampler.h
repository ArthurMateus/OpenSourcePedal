#pragma once

// Oversampler: a cascade of 2x halfband stages (ADR-0005 D1, oversampling.md §3.1). Pure C++20: no JUCE, no OS
// headers, and no HIIR types in this header (ADR-0006: HIIR is a PRIVATE dependency of osp_dsp_core).
//
// IIR (Live): HIIR 1.50 two-path polyphase allpass halfbands (L. de Soras, http://ldesoras.fr/prod.html#src_hiir,
// WTFPL-2.0), after Valenzuela & Constantinides, "Digital Signal Processing Schemes for Efficient Interpolation and
// Decimation", IEE Proceedings G 130(6), 1983. Coefficients per ADR-0005 D3 (JUCE's max-quality specs): stage 0 (the
// fs <-> 2fs stage) 90 dB / 0.05 up and 75 dB / 0.01 down (Amendment A1), later stages +10 dB per stage with
// transitions 0.10 up and 0.12 down.
// They come from HIIR's designer (tools/design/hiir_coefs.cpp) and are committed in src/oversampling/Oversampler.cpp.
// The FIR (HQ) kind is #63 part 3.
//
// The factor is chosen in prepare(), never in process() (ADR-0005 D1, REQ-OS-006). Usage per block:
//   float* os = oversampler.upsample(in, n);   // n * factor samples at the oversampled rate
//   ...process os in place...
//   oversampler.downsample(out, n);
// or oversampler.process(in, out, n, [](float* os, int osN) { ... }), which also splits blocks > maxBlockSize.

#include <memory>

namespace osp::dsp {

class Oversampler {
public:
    static constexpr int kMaxFactor = 8;
    static constexpr int kNumIirStages = 3;

    Oversampler();
    ~Oversampler();
    Oversampler(const Oversampler&) = delete;
    Oversampler& operator=(const Oversampler&) = delete;

    // Non-RT: allocates. factor is 1, 2, 4 or 8 (others round up to the next of these, capped at 8). Measures the
    // identity up/down delay at 1 kHz (ADR-0005 D4) and leaves the filters reset.
    void prepare(double sampleRate, int maxBlockSize, int factor);
    // RT-safe. Clears the filter state, so two renders after reset() are bit-identical.
    void reset() noexcept;

    // RT-safe. numSamples in [0, maxBlockSize]; larger counts are clamped. Returns the internal buffer holding
    // numSamples * factor samples, valid until the next upsample(). in may alias the out of a later downsample().
    // Returns nullptr before prepare() (process() handles that case).
    float* upsample(const float* in, int numSamples) noexcept;
    // RT-safe. Decimates the buffer of the last upsample() into numSamples outputs (same count as that call).
    // Before prepare() it zero-fills out.
    void downsample(float* out, int numSamples) noexcept;

    // RT-safe. Up -> fn(os, numSamples * factor) -> down, in chunks of at most maxBlockSize. in == out is allowed.
    template <typename Fn>
    void process(const float* in, float* out, int numSamples, Fn&& fn) noexcept
    {
        if (!isPrepared()) {
            downsample(out, numSamples);
            return;
        }
        for (int pos = 0; pos < numSamples; pos += maxBlock_) {
            const int n = numSamples - pos < maxBlock_ ? numSamples - pos : maxBlock_;
            fn(upsample(in + pos, n), n * factor_);
            downsample(out + pos, n);
        }
    }

    int getFactor() const noexcept { return factor_; }
    // round(D), D = the measured 1 kHz group delay of up + down, in base-rate samples (ADR-0005 D4).
    int getLatencySamples() const noexcept { return latency_; }
    double getMeasuredDelaySamples() const noexcept { return delay_; }
    bool isPrepared() const noexcept { return maxBlock_ > 0; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int factor_ = 1, maxBlock_ = 0, latency_ = 0, lastN_ = 0;
    double delay_ = 0.0;
    float* os_ = nullptr; // buffer holding the last upsample() result
};

namespace detail {
// Non-RT test hook (REQ-OS-002): runs IIR halfband stage `stage` (0 = the fs <-> 2fs stage) alone, from reset.
// up: numSamples in -> 2 * numSamples out. down: 2 * numSamples in -> numSamples out.
void runIirStage(int stage, bool up, const float* in, float* out, int numSamples);
} // namespace detail

} // namespace osp::dsp
