#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Zero-latency mono FIR convolution for the cab IR slots (`cab.*`, parameters.md §3.1). Latency 0 (signal-flow §4,
// L_cab = 0).
//
// Algorithm: non-uniform partitioned convolution in the scheme of W. G. Gardner, "Efficient Convolution without
// Input-Output Delay", J. Audio Eng. Soc. 43(3), 1995: a direct-form FIR head covers the first kHeadLength taps, and
// uniform partitioned overlap-save stages (FFT size 2B, frequency-domain delay line as in F. Wefers, "Partitioned
// convolution algorithms for real-time auralization", PhD thesis, RWTH Aachen, 2015, ch. 5) cover the rest with
// B = 64, 256, 1024. Every stage starts at a tap offset equal to its own partition size B, so the block computed when
// an input frame completes is exactly the output of the next B samples: no added delay, and no output buffering
// beyond one frame per stage. The 1024 cap bounds the per-callback peak (one 2048-point FFT pair plus the delay-line
// sum lands in the callback that completes a frame); it is not time-distributed in v1.
//
// FFT: Signalsmith `dsp` RealFFT<float> (MIT, ADR-0006 Amendment A1). Its packed layout keeps the Nyquist bin in the
// imaginary part of bin 0, and it is unnormalised both ways, so the partition spectra carry the 1/(2B) factor.

#include <memory>
#include <vector>

namespace osp::dsp {

class PartitionedConvolver {
public:
    // Direct-form head length, also the smallest FFT partition and the internal chunk length.
    static constexpr int kHeadLength = 64;
    static constexpr int kMaxPartition = 1024;

    PartitionedConvolver();
    ~PartitionedConvolver();
    PartitionedConvolver(const PartitionedConvolver&) = delete;
    PartitionedConvolver& operator=(const PartitionedConvolver&) = delete;

    // Non-RT: allocates. Copies the IR (any length >= 0; 0 gives silence), builds the partition spectra and clears the
    // state. The IR must already be at the processing sample rate.
    void prepare(const float* ir, int length);
    // RT-safe. Clears the input history and every delay line: two renders after reset() are bit-identical.
    void reset() noexcept;

    // RT-safe. in == out is allowed. Any numSamples >= 0, independent of a host block size. Before prepare() it
    // zero-fills the output.
    void process(const float* in, float* out, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return prepared_; }
    int irLength() const noexcept { return irLength_; }
    int numStages() const noexcept { return static_cast<int>(stages_.size()); }

private:
    struct Stage;

    std::vector<float> headReversed_; // kHeadLength taps, time-reversed and zero-padded
    std::vector<float> history_; // kHeadLength - 1 previous inputs, then the current chunk
    std::vector<std::unique_ptr<Stage>> stages_;
    int phase_ = 0; // position inside the current kHeadLength chunk
    int irLength_ = 0;
    bool prepared_ = false;
};

} // namespace osp::dsp
