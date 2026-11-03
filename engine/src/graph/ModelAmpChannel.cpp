#include "ModelAmpChannel.h"

#include <algorithm>
#include <cmath>

namespace osp::engine {

namespace {

constexpr float kMaxInput = 1.0e6f; // larger or non-finite input samples are treated as 0 (as ClassicAmpModel)
constexpr double kLowCutQ = 0.70710678118654752; // Butterworth

float clampLowCut(float hz) noexcept
{
    // NaN -> off
    return hz > ModelAmpChannel::kLowCutOffHz ? std::min(hz, ModelAmpChannel::kLowCutMaxHz)
                                              : ModelAmpChannel::kLowCutOffHz;
}

} // namespace

ModelAmpChannel::ModelAmpChannel(ClassicAmpId initial) : to_(static_cast<int>(index(initial))), wanted_(to_)
{
    from_ = to_;
    for (int k = 0; k < kNumModels; ++k)
        models_[static_cast<std::size_t>(k)] = std::make_unique<ClassicAmpModel>(static_cast<ClassicAmpId>(k));
}

void ModelAmpChannel::Pad::prepare(double pad, int maxDelay)
{
    total = std::max(pad, 0.0);
    // Integer part n and allpass part d in [0.5, 1.5) where the pad allows it: the first-order Thiran allpass is
    // most accurate (and its pole a furthest from the unit circle) around d = 1.
    const int n = total >= 0.5 ? static_cast<int>(std::floor(total - 0.5)) : 0;
    const double d = total - n;
    allpass = d > 1.0e-6;
    a = allpass ? static_cast<float>((1.0 - d) / (1.0 + d)) : 0.0f;
    line.prepare(std::max(maxDelay, n));
    line.setDelay(n);
    reset();
}

void ModelAmpChannel::Pad::process(float* io, int n) noexcept
{
    line.process(io, io, n);
    if (!allpass)
        return;
    for (int i = 0; i < n; ++i) { // y[n] = a x[n] + x[n-1] - a y[n-1]
        const float x = io[i];
        float y = a * (x - y1) + x1;
        if (std::abs(y) < 1.0e-20f) // no subnormals in silence, even with FTZ off
            y = 0.0f;
        x1 = x;
        y1 = y;
        io[i] = y;
    }
}

void ModelAmpChannel::prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality)
{
    fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maxBlockSize);
    for (auto* b : {&in_, &a_, &b_})
        b->assign(static_cast<std::size_t>(maxBlock_), 0.0f); // pre-touched (realtime-rules RT-12)

    double maxDelay = 0.0;
    for (auto& m : models_) {
        m->setParameters(params_);
        m->prepare(fs_, maxBlock_, quality);
        maxDelay = std::max(maxDelay, m->getMeasuredDelaySamples());
    }
    // ADR-0011 D2/A2: one integer slot delay for every model at (fs, quality). The 1e-9 guard keeps an exactly
    // integer processing delay from rounding up a whole sample on floating-point noise.
    slotDelay_ = std::ceil(maxDelay - 1.0e-9);
    for (std::size_t k = 0; k < models_.size(); ++k) {
        const double pad = slotDelay_ - models_[k]->getMeasuredDelaySamples();
        pads_[k].prepare(pad, static_cast<int>(std::ceil(pad)) + 1);
    }
    // The graph's hard-bypass pad is round(D_os1 + D_slot) - round(D_os1) = D_slot for an integer D_slot.
    bypass_.prepare(static_cast<int>(slotDelay_) + 2);

    fade_.prepare(fs_, kSwapSeconds);
    lowCutHz_.prepare(fs_, kLowCutSeconds);
    lowCutOn_.prepare(fs_, kLowCutSeconds);
    reset();
}

void ModelAmpChannel::reset() noexcept OSP_RT_NONBLOCKING
{
    from_ = to_ = wanted_; // a pending change is installed directly: reset() is a cold start
    models_[static_cast<std::size_t>(to_)]->setParameters(params_);
    models_[static_cast<std::size_t>(to_)]->reset();
    for (auto& p : pads_)
        p.reset();
    fade_.reset(1.0f);
    lowCutHz_.reset(lowCutHz_.target());
    lowCutOn_.reset(lowCutOn_.target());
    lowCutState_ = {};
    lowCutDesignedHz_ = 0.0f;
    bypass_.reset();
}

void ModelAmpChannel::setParameters(const AmpParams& params) noexcept OSP_RT_NONBLOCKING
{
    params_ = params;
    models_[static_cast<std::size_t>(to_)]->setParameters(params);
    if (from_ != to_)
        models_[static_cast<std::size_t>(from_)]->setParameters(params);
    const float hz = clampLowCut(params.lowCutHz);
    lowCutHz_.setTarget(hz);
    lowCutOn_.setTarget(hz > kLowCutOffHz ? 1.0f : 0.0f);
}

void ModelAmpChannel::setBypassed(bool bypassed, int padSamples) noexcept OSP_RT_NONBLOCKING
{
    bypassed_ = bypassed;
    bypass_.setDelay(std::max(padSamples, 0));
}

void ModelAmpChannel::setModel(ClassicAmpId id) noexcept OSP_RT_NONBLOCKING
{
    const auto k = static_cast<int>(index(id));
    if (k >= 0 && k < kNumModels)
        wanted_ = k;
}

void ModelAmpChannel::startPendingSwap() noexcept
{
    if (fade_.isRamping())
        return;
    from_ = to_;
    if (wanted_ == to_)
        return;
    to_ = wanted_;
    auto& incoming = *models_[static_cast<std::size_t>(to_)];
    incoming.setParameters(params_);
    incoming.reset(); // cold start at the current knob values (no glide from stale targets)
    pads_[static_cast<std::size_t>(to_)].reset();
    fade_.reset(0.0f);
    fade_.setTarget(1.0f);
}

void ModelAmpChannel::processLowCut(float* io, int n) noexcept
{
    if (!lowCutOn_.isRamping() && lowCutOn_.current() == 0.0f) {
        // Settled off: the exact input. The frequency follows to the off end, so a later on starts from 20 Hz.
        lowCutHz_.reset(lowCutHz_.target());
        lowCutState_ = {};
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float hz = lowCutHz_.next();
        if (hz != lowCutDesignedHz_) { // per sample while the frequency moves, once when it has settled
            lowCutCoeffs_ = dsp::svfDesign(dsp::SvfType::highPass, fs_, hz, kLowCutQ, 0.0);
            lowCutDesignedHz_ = hz;
        }
        const float x = io[i], y = lowCutState_.tick(lowCutCoeffs_, x);
        const float m = lowCutOn_.next();
        io[i] = m == 1.0f ? y : x + m * (y - x);
    }
}

void ModelAmpChannel::process(float* io, int numSamples) noexcept OSP_RT_NONBLOCKING
{
    const int n = std::min(numSamples, maxBlock_);
    if (n <= 0)
        return;
    if (bypassed_) {
        bypass_.process(io, io, n);
        return;
    }
    startPendingSwap();
    float* const in = in_.data();
    for (int i = 0; i < n; ++i)
        in[i] = std::abs(io[i]) <= kMaxInput ? io[i] : 0.0f;
    processLowCut(in, n);

    const auto to = static_cast<std::size_t>(to_);
    if (!fade_.isRamping()) {
        models_[to]->process(in, io, n);
        pads_[to].process(io, n);
        return;
    }
    const auto from = static_cast<std::size_t>(from_);
    float* const a = a_.data();
    float* const b = b_.data();
    models_[from]->process(in, a, n);
    pads_[from].process(a, n);
    models_[to]->process(in, b, n);
    pads_[to].process(b, n);
    for (int i = 0; i < n; ++i) { // linear: same input, correlated outputs (ADR-0004 §5)
        const float g = fade_.next();
        io[i] = (1.0f - g) * a[i] + g * b[i];
    }
}

} // namespace osp::engine
