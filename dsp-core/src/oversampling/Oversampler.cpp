#include "osp/dsp/oversampling/Oversampler.h"

// HIIR's portable FPU classes on every target. The single-channel SSE classes were about 1.7x faster in a scratch
// bench, but the SSE upsampler holds a zero-input limit cycle: after noise, the 8x domain keeps a period-4 tone at
// -113.5 dBFS (2x/4x: -138 dBFS) that never decays, even with FTZ/DAZ on. The FPU classes decay to exactly 0 (#96
// review, acct3). At 4x the FPU cost is about 0.1 % of a core (osp-bench oversampler_iir_4x).
#include "hiir/Downsampler2xFpu.h"
#include "hiir/Upsampler2xFpu.h"

#include "HalfbandFir.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <iterator>
#include <numbers>
#include <vector>

namespace osp::dsp {

namespace {

template <int NC>
using HiirUp = hiir::Upsampler2xFpu<NC>;
template <int NC>
using HiirDown = hiir::Downsampler2xFpu<NC>;

// hiir::PolyphaseIir2Designer::compute_coefs(coefs, attenuationDb, transition), HIIR 1.50, printed by
// tools/design/hiir_coefs.cpp. Transition is relative to the stage's higher rate (ADR-0005 D3).
// Stage 0 (fs <-> 2fs).
constexpr double kUp0[] = {0.045728148016233122, 0.16808754571240941, 0.33250111173947311, 0.50448575028293263,
                           0.66320202241939952,  0.80378086791112258, 0.93385580374462795}; // 90 dB, 0.05
constexpr double kDown0[] = {0.054217525755190787, 0.1967979698143128,  0.38308732729094908,
                             0.57313641114809133,  0.74872094443642634, 0.91429370968501777}; // 75 dB, 0.06
// Stage 1 (2fs <-> 4fs).
constexpr double kUp1[] = {0.039151597734459989, 0.14737711360104649, 0.3026468483284932,
                           0.48246854276969942,  0.67461591854696368, 0.88300502576937012}; // 100 dB, 0.10
constexpr double kDown1[] = {0.04790845477363561, 0.17951160525949356, 0.36780928458752388, 0.59071801647357025,
                             0.84841479122482688}; // 85 dB, 0.12
// Stage 2 (4fs <-> 8fs).
constexpr double kUp2[] = {0.029562017791614166, 0.11289255690954206, 0.23647102099689202, 0.3845800164514096,
                           0.54532365107113168,  0.71454214971259888, 0.89793653077981028}; // 110 dB, 0.10
constexpr double kDown2[] = {0.034527243895164733, 0.13163507855039655, 0.27561532380260084,
                             0.45007523683810918,  0.64646161470281949, 0.87042547098933665}; // 95 dB, 0.12

constexpr int size(const auto& a)
{
    return static_cast<int>(std::size(a));
}

struct IirCascade {
    HiirUp<size(kUp0)> up0;
    HiirUp<size(kUp1)> up1;
    HiirUp<size(kUp2)> up2;
    HiirDown<size(kDown0)> down0;
    HiirDown<size(kDown1)> down1;
    HiirDown<size(kDown2)> down2;

    IirCascade() noexcept
    {
        up0.set_coefs(kUp0);
        up1.set_coefs(kUp1);
        up2.set_coefs(kUp2);
        down0.set_coefs(kDown0);
        down1.set_coefs(kDown1);
        down2.set_coefs(kDown2);
    }

    // n = input samples of the stage (> 0).
    void up(int stage, float* dst, const float* src, long n) noexcept
    {
        if (stage == 0)
            up0.process_block(dst, src, n);
        else if (stage == 1)
            up1.process_block(dst, src, n);
        else
            up2.process_block(dst, src, n);
    }

    // n = output samples of the stage (> 0).
    void down(int stage, float* dst, const float* src, long n) noexcept
    {
        if (stage == 0)
            down0.process_block(dst, src, n);
        else if (stage == 1)
            down1.process_block(dst, src, n);
        else
            down2.process_block(dst, src, n);
    }

    void clear() noexcept
    {
        up0.clear_buffers();
        up1.clear_buffers();
        up2.clear_buffers();
        down0.clear_buffers();
        down1.clear_buffers();
        down2.clear_buffers();
    }
};

} // namespace

struct Oversampler::Impl {
    IirCascade iir;
    fir::FirCascade fir;
    std::vector<float> a, b; // ping-pong buffers, maxBlock * factor each
};

Oversampler::Oversampler() : impl_(std::make_unique<Impl>()) {}
Oversampler::~Oversampler() = default;

void Oversampler::prepare(double sampleRate, int maxBlockSize, int factor, OversamplingFilter filter)
{
    filter_ = filter;
    factor_ = factor <= 1 ? 1 : factor <= 2 ? 2 : factor <= 4 ? 4 : kMaxFactor;
    maxBlock_ = std::max(maxBlockSize, 1);
    const auto size = static_cast<std::size_t>(maxBlock_) * static_cast<std::size_t>(factor_);
    impl_->a.assign(size, 0.0f); // assign() writes every sample: the pre-touch of RT-12
    impl_->b.assign(size, 0.0f);
    reset();

    // ADR-0005 D4: D = group delay of the identity impulse response at 1 kHz, exactly Re{sum n h[n] e^-jwn / H(w)}.
    constexpr int kIrLength = 1024; // the slowest pole decays by e^-70 over this length at 2x
    std::vector<float> h(kIrLength, 0.0f);
    h[0] = 1.0f;
    process(h.data(), h.data(), kIrLength, [](float*, int) {});
    const double w = 2.0 * std::numbers::pi * 1000.0 / (sampleRate > 0.0 ? sampleRate : 48000.0);
    std::complex<double> num, den;
    for (int n = 0; n < kIrLength; ++n) {
        const auto e = std::polar(static_cast<double>(h[static_cast<std::size_t>(n)]), -w * n);
        num += static_cast<double>(n) * e;
        den += e;
    }
    delay_ = (num / den).real();
    latency_ = static_cast<int>(std::lround(delay_));
    reset();
}

void Oversampler::reset() noexcept
{
    impl_->iir.clear();
    impl_->fir.clear();
    lastN_ = 0;
    os_ = impl_->a.empty() ? nullptr : impl_->a.data();
}

float* Oversampler::upsample(const float* in, int numSamples) noexcept
{
    if (!isPrepared())
        return nullptr;
    const int n = std::clamp(numSamples, 0, maxBlock_);
    lastN_ = n;
    float* dst = impl_->a.data();
    if (factor_ == 1) {
        std::copy_n(in, n, dst);
        return os_ = dst;
    }
    const float* src = in;
    for (int stage = 0, len = n; (1 << stage) < factor_ && len > 0; ++stage, len *= 2) {
        dst = stage % 2 == 0 ? impl_->a.data() : impl_->b.data();
        if (filter_ == OversamplingFilter::fir)
            impl_->fir.up(stage, dst, src, len);
        else
            impl_->iir.up(stage, dst, src, len);
        src = dst;
    }
    return os_ = dst;
}

void Oversampler::downsample(float* out, int numSamples) noexcept
{
    const int n = std::clamp(numSamples, 0, maxBlock_);
    if (!isPrepared()) {
        std::fill_n(out, std::max(numSamples, 0), 0.0f);
        return;
    }
    assert(n == lastN_ && "downsample() must decimate the block of the last upsample()");
    if (n == 0)
        return;
    if (factor_ == 1) {
        std::copy_n(os_, n, out);
        return;
    }
    int stage = 0;
    while ((2 << stage) < factor_)
        ++stage; // the last up stage
    const float* src = os_;
    for (; stage >= 0; --stage) {
        float* dst = stage == 0 ? out : (src == impl_->a.data() ? impl_->b.data() : impl_->a.data());
        if (filter_ == OversamplingFilter::fir)
            impl_->fir.down(stage, dst, src, static_cast<long>(n) << stage);
        else
            impl_->iir.down(stage, dst, src, static_cast<long>(n) << stage);
        src = dst;
    }
}

void detail::runIirStage(int stage, bool up, const float* in, float* out, int numSamples)
{
    if (numSamples <= 0)
        return;
    const auto iir = std::make_unique<IirCascade>();
    const int s = std::clamp(stage, 0, Oversampler::kNumIirStages - 1);
    if (up)
        iir->up(s, out, in, numSamples);
    else
        iir->down(s, out, in, numSamples);
}

void detail::runFirStage(int stage, bool up, const float* in, float* out, int numSamples)
{
    if (numSamples <= 0)
        return;
    const auto f = std::make_unique<fir::FirCascade>();
    const int s = std::clamp(stage, 0, Oversampler::kNumIirStages - 1);
    if (up)
        f->up(s, out, in, numSamples);
    else
        f->down(s, out, in, numSamples);
}

} // namespace osp::dsp
