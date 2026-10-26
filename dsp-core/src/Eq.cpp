#include "osp/dsp/Eq.h"

#include <cmath>

namespace osp::dsp {

namespace {

constexpr double kSmoothSeconds = 0.020; // parameters.md §3.2: every eq parameter is smoothed 20 ms, xf 20

// Maps NaN to lo, so a broken host value can never reach a filter coefficient.
float clampf(float v, float lo, float hi) noexcept
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

void copy(const float (&src)[Eq::kMaxChannels][64], float (&dst)[Eq::kMaxChannels][64], int numChannels, int n) noexcept
{
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
            dst[ch][i] = src[ch][i];
}

// out = (1 - g)·a + g·b per sample, with g from `ramp` (advanced once per sample for all channels).
void mix(LinearRamp& ramp,
         float (&a)[Eq::kMaxChannels][64],
         const float (&b)[Eq::kMaxChannels][64],
         int numChannels,
         int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        const float g = ramp.next();
        for (int ch = 0; ch < numChannels; ++ch)
            a[ch][i] = (1.0f - g) * a[ch][i] + g * b[ch][i];
    }
}

} // namespace

void Eq::Band::set(double hz, float dB, double q) noexcept
{
    const auto lhz = static_cast<float>(std::log(hz));
    const auto lq = static_cast<float>(std::log(q));
    if (lhz != logHz.target() || dB != gainDb.target() || lq != logQ.target())
        dirty = true;
    logHz.setTarget(lhz);
    gainDb.setTarget(dB);
    logQ.setTarget(lq);
}

void Eq::Band::restart() noexcept
{
    logHz.reset(logHz.target());
    gainDb.reset(gainDb.target());
    logQ.reset(logQ.target());
    for (auto& st : s)
        st = {};
    dirty = true;
}

void Eq::Band::process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept
{
    if (logHz.isSmoothing() || gainDb.isSmoothing() || logQ.isSmoothing()) {
        for (int i = 0; i < n; ++i) {
            const double hz = std::exp(static_cast<double>(logHz.next()));
            const double q = std::exp(static_cast<double>(logQ.next()));
            c = svfDesign(type, sampleRate, hz, q, gainDb.next());
            for (int ch = 0; ch < numChannels; ++ch)
                buf[ch][i] = s[ch].tick(c, buf[ch][i]);
        }
        dirty = false;
        return;
    }
    if (dirty) {
        c = svfDesign(type, sampleRate, std::exp(static_cast<double>(logHz.current())),
                      std::exp(static_cast<double>(logQ.current())), gainDb.current());
        dirty = false;
    }
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
            buf[ch][i] = s[ch].tick(c, buf[ch][i]);
}

void Eq::Edge::process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept
{
    if (!toShelf.isRamping()) {
        (toShelf.current() > 0.5f ? shelf : peak).process(buf, numChannels, n, sampleRate);
        return;
    }
    Chunk alt;
    copy(buf, alt, numChannels, n);
    peak.process(buf, numChannels, n, sampleRate);
    shelf.process(alt, numChannels, n, sampleRate);
    mix(toShelf, buf, alt, numChannels, n);
}

void Eq::Cut::process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept
{
    Chunk filtered;
    copy(buf, filtered, numChannels, n);
    filter.process(filtered, numChannels, n, sampleRate); // always runs, so the state is warm when it fades in
    if (on.isRamping())
        mix(on, buf, filtered, numChannels, n);
    else if (on.current() > 0.5f)
        copy(filtered, buf, numChannels, n);
}

Eq::Eq() noexcept
{
    lowCut_.filter.type = SvfType::highPass;
    highCut_.filter.type = SvfType::lowPass;
    p1_.shelf.type = SvfType::lowShelf;
    p4_.shelf.type = SvfType::highShelf;
    for (int b = 0; b < 9; ++b)
        setGraphic9Db(b, 0.0f);
    for (int b = 0; b < 5; ++b)
        setGraphic5Db(b, 0.0f);
    const float defaultHz[4] = {100.0f, 500.0f, 2000.0f, 6000.0f};
    for (int b = 0; b < 4; ++b)
        setParametric(b, defaultHz[b], 0.0f, 0.7f);
    setEdgeShape(0, EqEdgeShape::shelf);
    setEdgeShape(3, EqEdgeShape::shelf);
    setLowCutHz(20.0f);
    setHighCutHz(20000.0f);
    level_.reset(1.0f);
}

void Eq::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    auto prepareBand = [&](Band& b) {
        b.logHz.prepare(sampleRate, kSmoothSeconds);
        b.gainDb.prepare(sampleRate, kSmoothSeconds);
        b.logQ.prepare(sampleRate, kSmoothSeconds);
    };
    for (auto& b : graphic9_)
        prepareBand(b);
    for (auto& b : graphic5_)
        prepareBand(b);
    for (Edge* e : {&p1_, &p4_}) {
        prepareBand(e->peak);
        prepareBand(e->shelf);
        e->toShelf.prepare(sampleRate, kSmoothSeconds);
    }
    prepareBand(p2_);
    prepareBand(p3_);
    for (Cut* c : {&lowCut_, &highCut_}) {
        prepareBand(c->filter);
        c->on.prepare(sampleRate, kSmoothSeconds);
    }
    modeFade_.prepare(sampleRate, kSmoothSeconds);
    level_.prepare(sampleRate, kSmoothSeconds);
    reset();
}

void Eq::reset() noexcept
{
    for (EqMode m : {EqMode::graphic9, EqMode::graphic5, EqMode::parametric})
        restartVoicing(m);
    for (Edge* e : {&p1_, &p4_})
        e->toShelf.reset(e->toShelf.target());
    for (Cut* c : {&lowCut_, &highCut_}) {
        c->filter.restart();
        c->on.reset(c->on.target());
    }
    modeFrom_ = modeTo_ = pendingMode_;
    modeFade_.reset(0.0f);
    level_.reset(level_.target());
}

void Eq::setMode(EqMode mode) noexcept
{
    pendingMode_ = mode;
    if (!isPrepared())
        modeFrom_ = modeTo_ = mode;
}

void Eq::setGraphic9Db(int band, float dB) noexcept
{
    if (band >= 0 && band < 9)
        graphic9_[band].set(kGraphic9Hz[band], clampf(dB, -12.0f, 12.0f), kGraphic9Q);
}

void Eq::setGraphic5Db(int band, float dB) noexcept
{
    if (band >= 0 && band < 5)
        graphic5_[band].set(kGraphic5Hz[band], clampf(dB, -12.0f, 12.0f), kGraphic5Q);
}

void Eq::setParametric(int band, float hz, float gainDb, float q) noexcept
{
    const double f = clampf(hz, 20.0f, 20000.0f), qq = clampf(q, 0.3f, 6.0f);
    const float g = clampf(gainDb, -15.0f, 15.0f);
    switch (band) {
    case 0:
        p1_.peak.set(f, g, qq);
        p1_.shelf.set(f, g, qq);
        break;
    case 1:
        p2_.set(f, g, qq);
        break;
    case 2:
        p3_.set(f, g, qq);
        break;
    case 3:
        p4_.peak.set(f, g, qq);
        p4_.shelf.set(f, g, qq);
        break;
    default:
        break;
    }
}

void Eq::setEdgeShape(int band, EqEdgeShape shape) noexcept
{
    Edge* e = band == 0 ? &p1_ : band == 3 ? &p4_ : nullptr;
    if (e == nullptr)
        return;
    const float target = shape == EqEdgeShape::shelf ? 1.0f : 0.0f;
    if (target != e->toShelf.target() && isPrepared())
        (target > 0.5f ? e->shelf : e->peak).restart(); // the incoming shape starts cold, faded in over 20 ms
    e->toShelf.setTarget(target);
}

void Eq::setLowCutHz(float hz) noexcept
{
    const float f = clampf(hz, 20.0f, 500.0f);
    lowCut_.filter.set(f, 0.0f, kCutQ);
    lowCut_.on.setTarget(f > 20.0f ? 1.0f : 0.0f);
}

void Eq::setHighCutHz(float hz) noexcept
{
    const float f = clampf(hz, 2000.0f, 20000.0f);
    highCut_.filter.set(f, 0.0f, kCutQ);
    highCut_.on.setTarget(f < 20000.0f ? 1.0f : 0.0f);
}

void Eq::setLevelDb(float dB) noexcept
{
    level_.setTarget(std::pow(10.0f, clampf(dB, -12.0f, 12.0f) / 20.0f));
}

void Eq::restartVoicing(EqMode mode) noexcept
{
    switch (mode) {
    case EqMode::graphic9:
        for (auto& b : graphic9_)
            b.restart();
        break;
    case EqMode::graphic5:
        for (auto& b : graphic5_)
            b.restart();
        break;
    case EqMode::parametric:
        for (Band* b : {&p1_.peak, &p1_.shelf, &p2_, &p3_, &p4_.peak, &p4_.shelf})
            b->restart();
        break;
    }
}

void Eq::processVoicing(EqMode mode, Chunk& buf, int numChannels, int n) noexcept
{
    switch (mode) {
    case EqMode::graphic9:
        for (auto& b : graphic9_)
            b.process(buf, numChannels, n, sampleRate_);
        break;
    case EqMode::graphic5:
        for (auto& b : graphic5_)
            b.process(buf, numChannels, n, sampleRate_);
        break;
    case EqMode::parametric:
        p1_.process(buf, numChannels, n, sampleRate_);
        p2_.process(buf, numChannels, n, sampleRate_);
        p3_.process(buf, numChannels, n, sampleRate_);
        p4_.process(buf, numChannels, n, sampleRate_);
        break;
    }
}

void Eq::processChunk(float* const* channels, int numChannels, int offset, int n) noexcept
{
    Chunk buf;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < n; ++i)
            buf[ch][i] = channels[ch][offset + i];

    lowCut_.process(buf, numChannels, n, sampleRate_);
    if (modeFrom_ != modeTo_) {
        Chunk incoming;
        copy(buf, incoming, numChannels, n);
        processVoicing(modeFrom_, buf, numChannels, n);
        processVoicing(modeTo_, incoming, numChannels, n);
        mix(modeFade_, buf, incoming, numChannels, n);
    } else {
        processVoicing(modeTo_, buf, numChannels, n);
    }
    highCut_.process(buf, numChannels, n, sampleRate_);

    for (int i = 0; i < n; ++i) {
        const float g = level_.next();
        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][offset + i] = buf[ch][i] * g;
    }
}

void Eq::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    const int nch = numChannels < kMaxChannels ? numChannels : kMaxChannels;
    if (numSamples <= 0 || nch <= 0)
        return;
    if (!isPrepared()) {
        for (int ch = 0; ch < nch; ++ch)
            for (int i = 0; i < numSamples; ++i)
                channels[ch][i] = 0.0f;
        return;
    }
    if (bypassed_)
        return;

    // Mode switches start at a block boundary, one at a time (xf 20: old and new run in parallel).
    if (!modeFade_.isRamping()) {
        if (modeFade_.current() > 0.0f)
            modeFrom_ = modeTo_;
        modeFade_.reset(0.0f);
        if (pendingMode_ != modeTo_) {
            modeFrom_ = modeTo_;
            modeTo_ = pendingMode_;
            restartVoicing(modeTo_);
            modeFade_.setTarget(1.0f);
        }
    }
    for (int pos = 0; pos < numSamples; pos += kChunk)
        processChunk(channels, nch, pos, numSamples - pos < kChunk ? numSamples - pos : kChunk);
}

} // namespace osp::dsp
