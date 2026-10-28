#include "osp/engine/Graph.h"

#include "GraphIds.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace osp::engine {

Graph::Graph(const ParamStore& params) : Graph(params, Options{}) {}

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
    for (auto* b : {&mono_, &dry_, &dryR_, &stripL_, &stripR_, &cleanBuf_, &postGain_})
        b->assign(n, 0.0f); // pre-touched (realtime-rules RT-12)

    input_.prepare(sampleRate, maxBlock_);
    tuner_.prepare(sampleRate, maxBlock_);
    gate_.prepare(sampleRate, maxBlock_);
    comp_.prepare(sampleRate, maxBlock_);
    drives_.prepare(sampleRate, maxBlock_, quality);
    mod_.prepare(sampleRate, maxBlock_);
    for (auto& s : strips_) {
        s.amp->prepare(sampleRate, maxBlock_, quality);
        s.cab.prepare(sampleRate, maxBlock_);
        s.eq.prepare(sampleRate, maxBlock_);
        s.eqOnL.prepare(sampleRate);
        s.eqOnR.prepare(sampleRate);
    }
    delay_.prepare(sampleRate, maxBlock_);
    reverb_.prepare(sampleRate, maxBlock_);
    doubler_.prepare(sampleRate, maxBlock_);
    output_.prepare(sampleRate, maxBlock_);
    inputOn_.prepare(sampleRate);
    compOn_.prepare(sampleRate);
    stripFade_.prepare(sampleRate, kSwapSeconds);
    tailIdleSamples_ = static_cast<int>(std::ceil(kTailIdleSeconds * sampleRate));
    modMoveSamples_ = static_cast<int>(std::ceil(dsp::Crossfader::kBlockEnableSeconds * sampleRate)) + 1;
    cleanBlend_.prepare(sampleRate, 0.020); // parameters.md §1.4: mix % is a 20 ms linear ramp
    gateOn_.prepare(sampleRate, dsp::Crossfader::kBlockEnableSeconds);
    gateTap_.prepare(sampleRate, 0.010); // parameters.md §2.2: gate.postAmp, xf 10

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
    const int gateDelay = R(dOs1 + dSlot) - R(0.0); // K_gate: from the gate slot to the amp output
    gatePost_.prepare(gateDelay);
    gatePost_.setDelay(gateDelay);
    setBypassedAll(bypassedAll_); // hands the new pad to the amp slots
    reset();
}

void Graph::reset() noexcept OSP_RT_NONBLOCKING
{
    applyParameters();
    input_.reset();
    tuner_.reset();
    gate_.reset();
    gateOn_.reset(gateOn_.target());
    gateTap_.reset(gateTap_.target());
    gatePost_.reset();
    comp_.reset();
    drives_.reset();
    mod_.reset();
    for (auto& s : strips_) {
        s.amp->reset();
        s.cab.reset();
        s.eq.reset();
        s.eqOnL.reset(s.eqOnL.target());
        s.eqOnR.reset(s.eqOnR.target());
    }
    delay_.reset();
    reverb_.reset();
    delayTail_ = {};
    reverbTail_ = {};
    doubler_.reset();
    output_.reset();
    inputOn_.reset(inputOn_.target());
    compOn_.reset(compOn_.target());
    clean_.reset();
    modPre_ = choice(ids_->modPosition) == 1;
    modMoveLeft_ = 0;
    cleanBlend_.reset(cleanBlend_.target());
    stripFrom_ = stripTo_ = choice(ids_->ampChannel);
    stripFade_.reset(1.0f);
}

void Graph::setBypassedAll(bool bypassed) noexcept OSP_RT_NONBLOCKING
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
    mod_.setBypassed(bypassed);
    delay_.setBypassed(bypassed);
    reverb_.setHardBypass(bypassed);
    doubler_.setBypassed(bypassed);
    output_.setBypassed(bypassed);
}

void Graph::setCabIr(int channel, int slot, dsp::PartitionedConvolver* ir) noexcept OSP_RT_NONBLOCKING
{
    if (channel >= 0 && channel < kNumChannels)
        strips_[static_cast<std::size_t>(channel)].cab.setIr(slot, ir);
}

dsp::PartitionedConvolver* Graph::popRetired() noexcept OSP_RT_NONBLOCKING
{
    for (auto& s : strips_)
        if (auto* ir = s.cab.popRetired())
            return ir;
    return nullptr;
}

void Graph::applyChannelSwitch() noexcept OSP_RT_NONBLOCKING
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

void Graph::applyModPosition(int numSamples) noexcept OSP_RT_NONBLOCKING
{
    const bool wantPre = choice(ids_->modPosition) == 1;
    if (modMoveLeft_ > 0) {
        modMoveLeft_ -= numSamples;
        if (modMoveLeft_ <= 0) { // the wet has faded out: move, cold-start, fade back in
            modMoveLeft_ = 0;
            modPre_ = wantPre;
            mod_.reset();
        }
    } else if (wantPre != modPre_) {
        modMoveLeft_ = modMoveSamples_;
    }
    mod_.setPosition(modPre_ ? dsp::Modulation::Position::pre : dsp::Modulation::Position::post);
    mod_.setEnabled(on(ids_->modEnabled) && modMoveLeft_ == 0);
}

namespace {

// Block enable for blocks without one (ADR-0009 D3): `dry` holds the block input, `io` the block output.
// Returns true when the block must run this chunk; a block that comes back from idle is reset first.
template <typename Block>
bool startEnable(dsp::Crossfader& xf, bool& idle, Block& block) noexcept OSP_RT_NONBLOCKING
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

// ADR-0009 D6. The gate computes g from its own input (the key). With h = 1 − e·(1 − g), the factor h^(1 − x)
// is applied here and h^x, delayed by K_gate, after the amp slot. Through a linear amp their product is h, aligned
// with the signal, for any x, so moving the tap is level-neutral. Settled factors are exact (no pow), and a
// disabled gate is exactly 1 on both sides. The delay line carries the attenuation 1 − h^x, so its cleared state
// after reset() is a factor of exactly 1.
void Graph::processGate(float* io, int n) noexcept OSP_RT_NONBLOCKING
{
    float* const post = postGain_.data();
    if (bypassedAll_) { // the gate has no hard bypass of its own: both factors are 1
        std::fill_n(post, n, 0.0f);
        gatePost_.process(post, post, n);
        std::fill_n(post, n, 1.0f);
        return;
    }
    float* const g = dry_.data();
    gate_.computeGain(io, g, n);
    for (int i = 0; i < n; ++i) {
        const float e = gateOn_.next(), x = gateTap_.next();
        const float h = std::max(1.0f - e * (1.0f - g[i]), 1.0e-6f);
        float pre = h, postFactor = 1.0f;
        if (x >= 1.0f) {
            pre = 1.0f;
            postFactor = h;
        } else if (x > 0.0f) {
            pre = std::pow(h, 1.0f - x);
            postFactor = std::pow(h, x);
        }
        io[i] *= pre;
        post[i] = 1.0f - postFactor;
    }
    gatePost_.process(post, post, n);
    for (int i = 0; i < n; ++i)
        post[i] = 1.0f - post[i];
}

// Trails with an idle state (see TailIdle). While disabled, the wet is out − in; once it has been below
// kTailSilence for tailIdleSamples_, the block is skipped, which leaves exactly the dry signal it was producing.
template <typename Run, typename Wake>
void Graph::processTail(TailIdle& t, bool enabled, float* l, float* r, int n, Run&& run, Wake&& wake) noexcept
    OSP_RT_NONBLOCKING
{
    if (t.idle) {
        if (!enabled || bypassedAll_)
            return;
        wake();
        t = {};
    }
    if (enabled || bypassedAll_) {
        t.silentSamples = 0;
        run();
        return;
    }
    std::copy_n(l, n, stripL_.data());
    std::copy_n(r, n, stripR_.data());
    run();
    float wet = 0.0f;
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        wet = std::max(wet, std::max(std::abs(l[i] - stripL_[k]), std::abs(r[i] - stripR_[k])));
    }
    t.silentSamples = wet <= kTailSilence ? t.silentSamples + n : 0;
    t.idle = t.silentSamples >= tailIdleSamples_;
}

void Graph::processStrip(Strip& s, const float* in, float* outL, float* outR, int n) noexcept OSP_RT_NONBLOCKING
{
    std::copy_n(in, n, outL);
    s.amp->process(outL, n);
    for (int i = 0; i < n; ++i) // gate.postAmp factor, aligned with the amp output (K_gate)
        outL[i] *= postGain_[static_cast<std::size_t>(i)];
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

void Graph::processChunk(float* io, float* outL, float* outR, int n) noexcept OSP_RT_NONBLOCKING
{
    float* const dry = dry_.data();
    if (startEnable(inputOn_, inputIdle_, input_)) {
        std::copy_n(io, n, dry);
        input_.process(io, io, n);
        inputOn_.process(dry, io, io, n);
    }
    tuner_.push(io, n);
    processGate(io, n);
    if (startEnable(compOn_, compIdle_, comp_)) {
        std::copy_n(io, n, dry);
        comp_.process(io, io, n);
        compOn_.process(dry, io, io, n);
    }
    drives_.process(io, io, n);
    if (modPre_)
        mod_.processMono(io, n); // pre-amp position: after drive2, before the clean DI tap (signal-flow §1)
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

    if (!modPre_)
        mod_.processStereo(outL, outR, n);
    processTail(
        delayTail_, on(ids_->delay[kDlyEnabled]), outL, outR, n,
        [&]() noexcept OSP_RT_NONBLOCKING { delay_.process(outL, outR, n); },
        [&]() noexcept OSP_RT_NONBLOCKING { // reset with the send still muted, so it fades in over its own 10 ms
            delay_.setSoftBypass(true);
            delay_.reset();
            delay_.setSoftBypass(false);
        });
    processTail(
        reverbTail_, on(ids_->reverb[kRevEnabled]), outL, outR, n,
        [&]() noexcept OSP_RT_NONBLOCKING { reverb_.process(outL, outR, outL, outR, n); },
        [&]() noexcept OSP_RT_NONBLOCKING {
            reverb_.setEnabled(false);
            reverb_.reset();
            reverb_.setEnabled(true);
        });
    doubler_.process(outL, outR, n);
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

void Graph::process(const float* in, float* outL, float* outR, int numSamples) noexcept OSP_RT_NONBLOCKING
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
        applyModPosition(n);
        std::copy_n(in + pos, n, mono_.data());
        processChunk(mono_.data(), outL + pos, outR + pos, n);
    }
}

} // namespace osp::engine
