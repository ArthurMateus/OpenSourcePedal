#include "osp/dsp/conv/PartitionedConvolver.h"

#include <signalsmith-dsp/fft.h>

#include <algorithm>
#include <cassert>
#include <complex>
#include <cstddef>

namespace osp::dsp {

namespace {

using Complex = std::complex<float>;
using Size = std::size_t;

// Packed product: bin 0 holds two real values (DC, Nyquist), the other bins are ordinary complex products.
void multiplyAccumulate(const Complex* x, const Complex* h, Complex* acc, int bins) noexcept
{
    acc[0] += Complex(x[0].real() * h[0].real(), x[0].imag() * h[0].imag());
    for (int k = 1; k < bins; ++k)
        acc[k] += x[k] * h[k];
}

} // namespace

// One uniform overlap-save stage: partitions of B taps starting at tap offset B, FFT size 2B.
struct PartitionedConvolver::Stage {
    Stage(const float* ir, int irLength, int partitionSize, int numPartitions)
        : size(partitionSize),
          partitions(numPartitions),
          fft(static_cast<Size>(2 * partitionSize)),
          filter(static_cast<Size>(numPartitions * partitionSize)),
          fdl(static_cast<Size>(numPartitions * partitionSize)),
          accum(static_cast<Size>(partitionSize)),
          input(static_cast<Size>(2 * partitionSize)),
          time(static_cast<Size>(2 * partitionSize)),
          output(static_cast<Size>(partitionSize))
    {
        const float scale = 1.0f / static_cast<float>(2 * size);
        for (int j = 0; j < partitions; ++j) {
            std::fill(time.begin(), time.end(), 0.0f);
            const int first = size + j * size; // the stage's offset equals B
            for (int m = 0; m < size && first + m < irLength; ++m)
                time[static_cast<Size>(m)] = ir[first + m] * scale;
            fft.fft(time.data(), filter.data() + static_cast<Size>(j * size));
        }
    }

    void clear() noexcept
    {
        std::fill(fdl.begin(), fdl.end(), Complex{});
        std::fill(input.begin(), input.end(), 0.0f);
        std::fill(output.begin(), output.end(), 0.0f);
        pos = 0;
        head = 0;
    }

    // Called when the current frame is complete: computes the output of the next B samples.
    void computeFrame() noexcept
    {
        fft.fft(input.data(), fdl.data() + static_cast<Size>(head * size));
        std::fill(accum.begin(), accum.end(), Complex{});
        for (int j = 0; j < partitions; ++j) {
            const int slot = (head - j + partitions) % partitions;
            multiplyAccumulate(fdl.data() + static_cast<Size>(slot * size), filter.data() + static_cast<Size>(j * size),
                               accum.data(), size);
        }
        fft.ifft(accum.data(), time.data());
        std::copy(time.begin() + size, time.end(), output.begin());
        std::copy(input.begin() + size, input.end(), input.begin());
        head = (head + 1) % partitions;
    }

    const int size;
    const int partitions;
    signalsmith::fft::RealFFT<float> fft;
    std::vector<Complex> filter; // partition spectra, packed, `size` bins each
    std::vector<Complex> fdl;    // frequency-domain delay line, ring of `partitions` input spectra
    std::vector<Complex> accum;
    std::vector<float> input;  // previous frame, then the current frame
    std::vector<float> time;   // 2B scratch
    std::vector<float> output; // output of the frame being played
    int pos = 0;               // samples of the current frame received so far
    int head = 0;              // fdl slot of the newest spectrum
};

PartitionedConvolver::PartitionedConvolver() = default;
PartitionedConvolver::~PartitionedConvolver() = default;

void PartitionedConvolver::prepare(const float* ir, int length)
{
    irLength_ = std::max(length, 0);
    headReversed_.assign(kHeadLength, 0.0f);
    for (int m = 0; m < kHeadLength && m < irLength_; ++m)
        headReversed_[static_cast<Size>(kHeadLength - 1 - m)] = ir[m];
    history_.assign(static_cast<Size>(2 * kHeadLength - 1), 0.0f);

    stages_.clear();
    int offset = kHeadLength;
    for (int size = kHeadLength; offset < irLength_; size = std::min(4 * size, kMaxPartition)) {
        assert(offset == size); // the zero-latency scheme needs every stage to start at offset B
        const int next = std::min(4 * size, kMaxPartition);
        const int end = next > size ? std::min(next, irLength_) : irLength_;
        const int partitions = (end - offset + size - 1) / size;
        stages_.push_back(std::make_unique<Stage>(ir, irLength_, size, partitions));
        offset += partitions * size;
        if (next == size)
            break;
    }
    prepared_ = true;
    reset();
}

void PartitionedConvolver::reset() noexcept
{
    std::fill(history_.begin(), history_.end(), 0.0f);
    for (auto& stage : stages_)
        stage->clear();
    setStartPhase(startPhase_);
}

void PartitionedConvolver::setStartPhase(int samples) noexcept
{
    startPhase_ = samples > 0 ? samples % kMaxPartition : 0;
    phase_ = startPhase_ % kHeadLength;
    for (auto& stage : stages_)
        stage->pos = startPhase_ % stage->size; // every size divides kMaxPartition, so the phases stay consistent
}

void PartitionedConvolver::process(const float* in, float* out, int numSamples) noexcept
{
    if (!prepared_) {
        std::fill(out, out + std::max(numSamples, 0), 0.0f);
        return;
    }
    constexpr int kTail = kHeadLength - 1;
    int done = 0;
    while (done < numSamples) {
        const int len = std::min(numSamples - done, kHeadLength - phase_);
        float* const chunk = history_.data() + kTail + phase_;
        std::copy(in + done, in + done + len, chunk);
        for (auto& stage : stages_)
            std::copy(in + done, in + done + len, stage->input.data() + stage->size + stage->pos);

        // Every input of this chunk is now copied, so writing `out` is safe when it aliases `in`.
        for (int i = 0; i < len; ++i) {
            const float* x = chunk + i - kTail;
            float acc = 0.0f;
            for (int k = 0; k < kHeadLength; ++k)
                acc += headReversed_[static_cast<Size>(k)] * x[k];
            out[done + i] = acc;
        }
        for (auto& stage : stages_) {
            const float* y = stage->output.data() + stage->pos;
            for (int i = 0; i < len; ++i)
                out[done + i] += y[i];
            stage->pos += len;
            if (stage->pos == stage->size) {
                stage->computeFrame();
                stage->pos = 0;
            }
        }

        phase_ += len;
        done += len;
        if (phase_ == kHeadLength) {
            std::copy(history_.end() - kTail, history_.end(), history_.begin());
            phase_ = 0;
        }
    }
}

} // namespace osp::dsp
