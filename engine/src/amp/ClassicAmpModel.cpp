#include "ClassicAmpModel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace osp::engine {

namespace {

constexpr float kMaxInput = 1.0e6f; // larger or non-finite input samples are treated as 0

double tptG(double hz, double fs) noexcept
{
    const double g = std::tan(std::numbers::pi * std::min(hz, 0.45 * fs) / fs);
    return g / (1.0 + g);
}

// One TPT one-pole low-pass step (Zavalishin 2018, §3.10) on double state.
double lowPass(double x, double G, double& s) noexcept
{
    const double v = (x - s) * G, lp = v + s;
    s = lp + v;
    if (std::abs(s) < 1e-30) // denormal-safe without FTZ (the state decays geometrically in silence)
        s = 0.0;
    return lp;
}

float dbToGain(double db) noexcept
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

float sanitise(float x) noexcept
{
    return std::abs(x) <= kMaxInput ? x : 0.0f;
}

// −d(arg H)/dω in samples of the rate H is evaluated at, by a central difference of ±0.5 Hz.
template <typename H>
double groupDelay(H&& h, double hz, double fs) noexcept
{
    constexpr double kDelta = 0.5;
    return -std::arg(h(hz + kDelta) / h(hz - kDelta)) / (2.0 * std::numbers::pi * 2.0 * kDelta / fs);
}

} // namespace

std::string_view classicAmpModelId(ClassicAmpId id) noexcept
{
    return dsp::amp::ampVoicing(static_cast<dsp::amp::AmpVoicingId>(id)).id;
}

std::unique_ptr<IAmpModel> makeClassicAmp(ClassicAmpId id)
{
    return std::make_unique<ClassicAmpModel>(id);
}

ClassicAmpModel::ClassicAmpModel(ClassicAmpId id)
    : id_(id),
      voicing_(dsp::amp::ampVoicing(static_cast<dsp::amp::AmpVoicingId>(id)))
{
    info_.architecture = "classic";
}

void ClassicAmpModel::prepare(double hostSampleRate, int maxBlockSize, dsp::OversamplingQuality quality)
{
    fs_ = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
    const int maxBlock = std::max(maxBlockSize, 1);
    const int factor = dsp::DriveRegion::factorFor(fs_, quality); // OS-2 uses the OS-1 table (ADR-0005 D2)
    const auto filter =
        quality == dsp::OversamplingQuality::hq ? dsp::OversamplingFilter::fir : dsp::OversamplingFilter::iir;
    os_.prepare(fs_, maxBlock, factor, filter);
    const double osRate = fs_ * factor;
    const auto& v = voicing_;

    for (int i = 0; i < v.numStages; ++i) {
        const auto& sv = v.stages[static_cast<std::size_t>(i)];
        auto& s = stages_[static_cast<std::size_t>(i)];
        s.prepare(osRate, sv.config);
        s.setCouplingHz(sv.couplingHz);
        s.setMillerHz(sv.millerHz);
        s.setCathodeHz(sv.cathodeHz);
        // The tone stack's makeup gain belongs to the grid of the stage after it.
        s.setDriveDb(sv.driveDb + static_cast<float>(i == v.toneStackAfter ? v.stackMakeupDb : 0.0));
    }
    stack_.setVariant(v.stack);
    stack_.prepare(osRate, maxBlock * factor);
    auto power = v.power;
    power.masterDbAt5 += v.powerInputDb + (v.toneStackAfter == v.numStages ? v.stackMakeupDb : 0.0);
    power_.setVoicing(power);
    power_.prepare(osRate, maxBlock * factor);

    brightG_ = tptG(v.brightHz, fs_);
    tightG_ = tptG(v.tightHz, fs_);
    brightMix_.prepare(fs_, kSwitchSeconds);
    tightMix_.prepare(fs_, kSwitchSeconds);
    level_.prepare(fs_, kLevelSeconds);

    // Delay at the parameters.md defaults, then back to the caller's parameters.
    const AmpParams current = params_;
    AmpParams defaults{};
    defaults.gain = static_cast<float>(id_ == ClassicAmpId::cleanA ? 3 : id_ == ClassicAmpId::crunchA ? 5 : 6);
    params_ = defaults;
    applyParameters();
    // Processing delay only (orchestrator decision on #191, the DriveRegion/NAM convention): the oversampler's measured
    // round trip plus the ADAA terms. The modelled filters' phase is part of the sound and is kept as a diagnostic.
    const double adaa = v.numStages * dsp::amp::PreampStage::kDelaySamples + dsp::amp::PowerAmp::kDelaySamples;
    delay_ = os_.getMeasuredDelaySamples() + adaa / factor;
    filterDelay_ = linearGroupDelay() - adaa / factor;
    latency_ = static_cast<int>(std::ceil(delay_ - 1e-9));
    params_ = current;
    applyParameters();
    reset();
}

double ClassicAmpModel::linearGroupDelay() const noexcept
{
    const double factor = os_.getFactor(), osRate = fs_ * factor;
    double d = 0.0;
    for (int i = 0; i < voicing_.numStages; ++i)
        d += stages_[static_cast<std::size_t>(i)].groupDelaySamples(1000.0);
    dsp::amp::ToneStackDesigner designer;
    designer.prepare(osRate);
    dsp::amp::ToneStackCoeffs c;
    if (designer.design(dsp::amp::toneStackComponents(voicing_.stack), params_.bass, params_.mid, params_.treble, c)) {
        d += groupDelay(
            [&](double hz) {
                const auto z1 = std::polar(1.0, -2.0 * std::numbers::pi * hz / osRate);
                std::complex<double> h = c.d;
                for (std::size_t i = 0; i < 3; ++i)
                    h += c.r[i] * z1 / (1.0 - c.z[i] * z1);
                return h;
            },
            1000.0, osRate);
    }
    d += groupDelay([&](double hz) { return power_.response(hz); }, 1000.0, osRate);
    return d / factor; // bright/tight are off at the defaults: no base-rate filter in the path
}

void ClassicAmpModel::reset() noexcept
{
    os_.reset();
    for (int i = 0; i < voicing_.numStages; ++i)
        stages_[static_cast<std::size_t>(i)].reset();
    stack_.reset();
    power_.reset();
    brightState_ = tightState_ = 0.0;
    brightMix_.reset(brightMix_.target());
    tightMix_.reset(tightMix_.target());
    level_.reset(level_.target());
}

void ClassicAmpModel::setParameters(const AmpParams& params) noexcept
{
    params_ = params;
    applyParameters();
}

void ClassicAmpModel::applyParameters() noexcept
{
    const auto& v = voicing_;
    const auto& p = params_;
    const float gain = p.gain >= 0.0f ? std::min(p.gain, 10.0f) : 0.0f; // NaN -> 0
    stages_[0].setDriveDb(v.gainDbAt0 + gain * v.gainDbPerKnob);
    for (int i = 0; i < v.numStages; ++i) {
        const auto& sv = v.stages[static_cast<std::size_t>(i)];
        if (sv.config.cathodeShelfDb < 0.0)
            stages_[static_cast<std::size_t>(i)].setCathodeHz(sv.cathodeHz + (p.tight ? v.tightCathodeHz : 0.0f));
    }
    stack_.setBass(p.bass);
    stack_.setMid(p.mid);
    stack_.setTreble(p.treble);
    power_.setPresence(p.presence);
    power_.setDepth(p.depth);
    power_.setMaster(p.master);
    const double shelfDb = v.brightDb * (1.0 - gain / 10.0);
    brightMix_.setTarget(p.bright ? dbToGain(shelfDb) - 1.0f : 0.0f);
    tightMix_.setTarget(p.tight ? 1.0f : 0.0f);
    const float levelDb = p.levelDb >= -24.0f ? std::min(p.levelDb, 12.0f) : -24.0f;
    level_.setTarget(dbToGain(levelDb + v.outputTrimDb));
}

void ClassicAmpModel::runOversampled(float* os, int n) noexcept
{
    const auto& v = voicing_;
    for (int i = 0; i < v.numStages; ++i) {
        stages_[static_cast<std::size_t>(i)].process(os, n);
        if (i + 1 == v.toneStackAfter)
            stack_.process(os, n);
    }
    power_.process(os, n);
}

void ClassicAmpModel::process(const float* in, float* out, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!isPrepared()) {
        std::fill_n(out, numSamples, 0.0f);
        return;
    }
    for (int n = 0; n < numSamples; ++n) {
        double x = static_cast<double>(sanitise(in[n])) * dsp::amp::kGridAt0Dbfs;
        x -= tightMix_.next() * lowPass(x, tightG_, tightState_);
        x += brightMix_.next() * (x - lowPass(x, brightG_, brightState_));
        out[n] = static_cast<float>(x);
    }
    os_.process(out, out, numSamples, [this](float* os, int n) { runOversampled(os, n); });
    for (int n = 0; n < numSamples; ++n)
        out[n] *= level_.next();
}

} // namespace osp::engine
