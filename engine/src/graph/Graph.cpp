#include "osp/engine/Graph.h"

#include "GraphIds.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace osp::engine {

Graph::Graph(const ParamStore& params, Options options)
    : store_(params),
      table_(paramTable()),
      ids_(std::make_unique<Ids>())
{
    for (auto& s : strips_)
        s.amp = std::make_unique<AmpStub>(options.ampStubDelaySamples);
    // Every block starts at the parameters.md defaults, with no fade from the constructor state.
    applyParameters();
    stripFrom_ = stripTo_ = choice(ids_->ampChannel);
}

Graph::~Graph()
{
    for (auto& s : strips_)
        s.cab.retireAll(); // the convolvers belong to the engine; they are handed back, never deleted here
}

void Graph::prepare(double sampleRate, int maxBlockSize, dsp::OversamplingQuality quality)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(1, maxBlockSize);
    const auto n = static_cast<std::size_t>(maxBlock_);
    for (auto* b : {&mono_, &dry_, &dryR_, &stripL_, &stripR_, &cleanBuf_})
        b->assign(n, 0.0f); // pre-touched (realtime-rules RT-12)

    input_.prepare(sampleRate, maxBlock_);
    tuner_.prepare(sampleRate, maxBlock_);
    gate_.prepare(sampleRate, maxBlock_);
    comp_.prepare(sampleRate, maxBlock_);
    drives_.prepare(sampleRate, maxBlock_, quality);
    for (auto& s : strips_) {
        s.amp->prepare(sampleRate, maxBlock_, quality);
        s.cab.prepare(sampleRate, maxBlock_);
        s.eq.prepare(sampleRate, maxBlock_);
        s.eqOnL.prepare(sampleRate);
        s.eqOnR.prepare(sampleRate);
    }
    output_.prepare(sampleRate, maxBlock_);
    inputOn_.prepare(sampleRate);
    compOn_.prepare(sampleRate);
    stripFade_.prepare(sampleRate, kSwapSeconds);
    cleanBlend_.prepare(sampleRate, 0.020); // parameters.md §1.4: mix % is a 20 ms linear ramp

    // ADR-0009 D4: P(x) = the measured delay from the input to tap x; each tap gets R(x) = round(P(x)) and a
    // parallel path from a to b is delayed by R(b) − R(a), so every path adds up to exactly L_total.
    const double dOs1 = drives_.getMeasuredDelaySamples();
    const double dSlot = strips_[0].amp->getMeasuredDelaySamples();
    for ([[maybe_unused]] const auto& s : strips_)
        assert(s.amp->getMeasuredDelaySamples() == dSlot); // ADR-0002 D4: one L_slot for every channel
    const double dCab = strips_[0].cab.getLatencySamples(); // 0: zero-latency convolution (signal-flow §4)
    const auto R = [](double p) { return static_cast<int>(std::lround(p)); };
    measuredDelay_ = dOs1 + dSlot + dCab;
    latency_ = R(measuredDelay_);
    slotPad_ = R(dOs1 + dSlot) - R(dOs1);
    const int cleanDelay = latency_ - R(dOs1); // K_clean
    clean_.prepare(cleanDelay);
    clean_.setDelay(cleanDelay);
    setBypassedAll(bypassedAll_); // hands the new pad to the amp slots
    reset();
}

void Graph::reset() noexcept
{
    applyParameters();
    input_.reset();
    tuner_.reset();
    gate_.reset();
    comp_.reset();
    drives_.reset();
    for (auto& s : strips_) {
        s.amp->reset();
        s.cab.reset();
        s.eq.reset();
        s.eqOnL.reset(s.eqOnL.target());
        s.eqOnR.reset(s.eqOnR.target());
    }
    output_.reset();
    inputOn_.reset(inputOn_.target());
    compOn_.reset(compOn_.target());
    clean_.reset();
    cleanBlend_.reset(cleanBlend_.target());
    stripFrom_ = stripTo_ = choice(ids_->ampChannel);
    stripFade_.reset(1.0f);
}

void Graph::setBypassedAll(bool bypassed) noexcept
{
    bypassedAll_ = bypassed;
    input_.setBypassed(bypassed);
    comp_.setBypassed(bypassed);
    drives_.setBypassed(bypassed); // an exact delay of round(D_os1) = R(amp in)
    for (auto& s : strips_) {
        s.amp->setBypassed(bypassed, slotPad_);
        s.cab.setBypassed(bypassed);
        s.eq.setBypassed(bypassed);
    }
    output_.setBypassed(bypassed);
}

void Graph::setCabIr(int channel, int slot, dsp::PartitionedConvolver* ir) noexcept
{
    if (channel >= 0 && channel < kNumChannels)
        strips_[static_cast<std::size_t>(channel)].cab.setIr(slot, ir);
}

dsp::PartitionedConvolver* Graph::popRetired() noexcept
{
    for (auto& s : strips_)
        if (auto* ir = s.cab.popRetired())
            return ir;
    return nullptr;
}

void Graph::applyChannelSwitch() noexcept
{
    if (stripFade_.isRamping())
        return; // a change during a fade starts when it ends
    stripFrom_ = stripTo_;
    const int wanted = choice(ids_->ampChannel);
    if (wanted == stripTo_ || maxBlock_ == 0)
        return;
    stripTo_ = wanted;
    auto& s = strips_[static_cast<std::size_t>(wanted)];
    s.amp->reset(); // cold start of the incoming strip, like an Eq voicing (ADR-0009 D1)
    s.cab.reset();
    s.eq.reset();
    stripFade_.reset(0.0f);
    stripFade_.setTarget(1.0f);
}

namespace {

// Block enable for blocks without one (ADR-0009 D3): `dry` holds the block input, `io` the block output.
// Returns true when the block must run this chunk; a block that comes back from idle is reset first.
template <typename Block>
bool startEnable(dsp::Crossfader& xf, bool& idle, Block& block) noexcept
{
    if (xf.isSettledOnA() && !xf.target()) {
        idle = true;
        return false;
    }
    if (idle)
        block.reset();
    idle = false;
    return true;
}

} // namespace

void Graph::processStrip(Strip& s, const float* in, float* outL, float* outR, int n) noexcept
{
    std::copy_n(in, n, outL);
    s.amp->process(outL, n);
    s.cab.process(outL, outL, outR, n);
    if (!startEnable(s.eqOnL, s.eqIdle, s.eq))
        return;
    std::copy_n(outL, n, dry_.data());
    std::copy_n(outR, n, dryR_.data());
    float* const lr[2] = {outL, outR};
    s.eq.process(lr, 2, n);
    s.eqOnL.process(dry_.data(), outL, outL, n);
    s.eqOnR.process(dryR_.data(), outR, outR, n);
}

void Graph::processChunk(float* io, float* outL, float* outR, int n) noexcept
{
    float* const dry = dry_.data();
    if (startEnable(inputOn_, inputIdle_, input_)) {
        std::copy_n(io, n, dry);
        input_.process(io, io, n);
        inputOn_.process(dry, io, io, n);
    }
    tuner_.push(io, n);
    if (!bypassedAll_) // the gate has no hard bypass of its own; it is skipped
        gate_.process(io, io, n);
    if (startEnable(compOn_, compIdle_, comp_)) {
        std::copy_n(io, n, dry);
        comp_.process(io, io, n);
        compOn_.process(dry, io, io, n);
    }
    drives_.process(io, io, n);
    float* const clean = cleanBuf_.data();
    clean_.process(io, clean, n); // always runs, so it is warm when the blend comes up

    auto& to = strips_[static_cast<std::size_t>(stripTo_)];
    if (stripFade_.isRamping()) {
        processStrip(strips_[static_cast<std::size_t>(stripFrom_)], io, stripL_.data(), stripR_.data(), n);
        processStrip(to, io, outL, outR, n);
        for (int i = 0; i < n; ++i) { // linear: same input, strongly correlated outputs (ADR-0004 §5)
            const float g = stripFade_.next();
            outL[i] = (1.0f - g) * stripL_[static_cast<std::size_t>(i)] + g * outL[i];
            outR[i] = (1.0f - g) * stripR_[static_cast<std::size_t>(i)] + g * outR[i];
        }
    } else {
        processStrip(to, io, outL, outR, n);
    }

    // out = (1 − β)·wet + β·clean (ADR-0009 D7). β = 0 settled leaves the wet untouched; β = 1 is exactly clean.
    if (cleanBlend_.isRamping() || cleanBlend_.current() != 0.0f) {
        for (int i = 0; i < n; ++i) {
            const float b = cleanBlend_.next();
            outL[i] = (1.0f - b) * outL[i] + b * clean[i];
            outR[i] = (1.0f - b) * outR[i] + b * clean[i];
        }
    }

    output_.process(outL, outR, n);
    // Tuner mute (REQ-419): one ramp for both channels. Unmuted and idle it leaves the gains at exactly 1.
    float* const gain = dry_.data();
    std::fill_n(gain, n, 1.0f);
    tuner_.applyMute(gain, n);
    for (int i = 0; i < n; ++i) {
        outL[i] *= gain[i];
        outR[i] *= gain[i];
    }
}

void Graph::process(const float* in, float* outL, float* outR, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (maxBlock_ == 0) {
        std::fill_n(outL, numSamples, 0.0f);
        std::fill_n(outR, numSamples, 0.0f);
        return;
    }
    applyParameters();
    for (int pos = 0; pos < numSamples; pos += maxBlock_) { // Engine splits host blocks; this is a safety net
        const int n = std::min(maxBlock_, numSamples - pos);
        applyChannelSwitch();
        std::copy_n(in + pos, n, mono_.data());
        processChunk(mono_.data(), outL + pos, outR + pos, n);
    }
}

} // namespace osp::engine
