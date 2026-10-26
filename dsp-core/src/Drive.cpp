#include "osp/dsp/Drive.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace osp::dsp {

namespace {

struct Voicing {
    float gainMinDb, gainMaxDb; // over drive 0…10
    float preHpHz;              // 0 = none
    float fullBand;             // c in u = G·(hp(x) + b) + c·x
    float inputBias;            // b, in the input domain (fuzz only)
    float postLpHz, outGain;
};

// Own engineering values, unverified (see Drive.h). The mid minimum (21.6 dB) is 1 + 51k/4.7k, its maximum (41.4 dB)
// adds the 500k drive pot to the 51k leg (tone-targets-b F1). kOut = 0.25 puts a saturated drive at the −12 dBFS DI
// nominal peak (tone-targets-a §3.4), so level 0 dB keeps the level roughly where it came in.
// - transparent is not a flat clean boost: at drive 0 with tone flat it measures −1.7 dB at 10 kHz and −5.7 dB at
//   20 kHz (4x at 44.1 kHz; #102 review), from the 16 kHz post-LPF plus ADAA2's (1 + 2·cos w)/3.
// - fuzz biases the input before the gain (b = 0.01, about −40 dBFS), so the clip threshold stays at x = −b and the
//   duty cycle, and with it the even harmonics, survives at every drive. A bias after the gain makes the wave a
//   symmetric square at high gain (H2 −57.6 dB at max drive; #102 review). At max drive, input below about −40 dBFS
//   stays at the rail: the gated "sputter" of a starved fuzz, by design.
constexpr Voicing kVoicings[] = {
    {21.6f, 41.4f, 720.0f, 1.0f, 0.0f, 6000.0f, 0.25f}, // mid
    {0.0f, 26.0f, 0.0f, 0.0f, 0.0f, 16000.0f, 1.0f},    // transparent
    {12.0f, 60.0f, 160.0f, 0.0f, 0.0f, 4500.0f, 0.25f}, // dist
    {20.0f, 60.0f, 100.0f, 0.0f, 0.01f, 7000.0f, 0.25f}, // fuzz
};
static_assert(std::size(kVoicings) == 4, "kVoicings is indexed by DriveType");

constexpr double kDcHz = 10.0;
constexpr double kToneHz = 1500.0, kBrightHz = 3000.0, kBrightDb = 6.0, kQ = 0.70710678118654752;
constexpr float kToneMinDb = -12.0f, kToneMaxDb = 6.0f; // high-shelf gain over tone 0…10; 5 = −3 dB

float onePoleG(double hz, double fs)
{
    const double g = std::tan(std::numbers::pi * std::min(hz, 0.49 * fs) / fs);
    return static_cast<float>(g / (1.0 + g));
}

float dbToGain(float dB)
{
    return std::pow(10.0f, dB / 20.0f);
}

} // namespace

DriveStage::DriveStage() noexcept
{
    drive_.reset(2.0f);
    tone_.reset(5.0f);
    lowCut_.reset(20.0f);
    level_.reset(1.0f);
    mix_.reset(1.0f);
    enable_.reset(1.0f);
}

void DriveStage::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_ = sampleRate;
    drive_.prepare(sampleRate, 0.030);
    tone_.prepare(sampleRate, 0.020);
    lowCut_.prepare(sampleRate, 0.020);
    level_.prepare(sampleRate, 0.020);
    mix_.prepare(sampleRate, 0.020);
    typeFade_.prepare(sampleRate, 0.010);
    bright_.prepare(sampleRate, 0.010);
    enable_.prepare(sampleRate, 0.010);
    for (std::size_t t = 0; t < paths_.size(); ++t) {
        paths_[t].pre.g = onePoleG(kVoicings[t].preHpHz, sampleRate);
        paths_[t].post.g = onePoleG(kVoicings[t].postLpHz, sampleRate);
        paths_[t].dc.g = onePoleG(kDcHz, sampleRate);
    }
    brightCoeffs_ = svfDesign(SvfType::highShelf, sampleRate, kBrightHz, kQ, kBrightDb);
    reset();
}

void DriveStage::reset() noexcept
{
    drive_.reset(drive_.target());
    tone_.reset(tone_.target());
    lowCut_.reset(lowCut_.target());
    level_.reset(level_.target());
    mix_.reset(mix_.target());
    bright_.reset(bright_.target());
    enable_.reset(enabled_ ? 1.0f : 0.0f);
    typeFrom_ = typeTo_ = pendingType_;
    typeFade_.reset(0.0f);
    for (int t = 0; t < 4; ++t)
        resetPath(static_cast<DriveType>(t));
    lowCutState_ = toneState_ = brightState_ = {};
    dry1_ = 0.0f;
    if (isPrepared()) {
        updateLowCut(lowCut_.current());
        updateTone(tone_.current());
    }
}

void DriveStage::setType(DriveType type) noexcept
{
    pendingType_ = type;
    if (!isPrepared()) {
        typeFrom_ = typeTo_ = type;
        return;
    }
    if (typeFrom_ != typeTo_ || type == typeTo_)
        return; // no change, or a fade is running: process() starts the queued change when it ends
    typeTo_ = type;
    resetPath(type);
    typeFade_.reset(0.0f);
    typeFade_.setTarget(1.0f);
}

void DriveStage::setDrive(float knob) noexcept
{
    drive_.setTarget(std::clamp(knob, 0.0f, 10.0f));
}
void DriveStage::setTone(float knob) noexcept
{
    tone_.setTarget(std::clamp(knob, 0.0f, 10.0f));
}
void DriveStage::setLevelDb(float dB) noexcept
{
    level_.setTarget(dbToGain(std::clamp(dB, -20.0f, 12.0f)));
}
void DriveStage::setLowCutHz(float hz) noexcept
{
    lowCut_.setTarget(std::clamp(hz, 20.0f, 300.0f));
}
void DriveStage::setBright(bool on) noexcept
{
    bright_.setTarget(on ? 1.0f : 0.0f);
}
void DriveStage::setMixPct(float pct) noexcept
{
    mix_.setTarget(std::clamp(pct, 0.0f, 100.0f) / 100.0f);
}

void DriveStage::setEnabled(bool on) noexcept
{
    if (on && !enabled_ && enable_.current() == 0.0f) { // waking from fully off: start from clean state
        for (int t = 0; t < 4; ++t)
            resetPath(static_cast<DriveType>(t));
        lowCutState_ = toneState_ = brightState_ = {};
    }
    enabled_ = on;
    enable_.setTarget(on ? 1.0f : 0.0f);
}

void DriveStage::updateGain(DriveType type, float knob) noexcept
{
    const auto t = static_cast<std::size_t>(type);
    const Voicing& v = kVoicings[t];
    Path& p = paths_[t];
    p.gainKnob = knob;
    p.gain = dbToGain(v.gainMinDb + (v.gainMaxDb - v.gainMinDb) * knob / 10.0f);
    // The shaper input at rest is exactly this float product (hp = 0 in voicing()), so rest matches it to the bit.
    p.restInput = p.gain * (0.0f + v.inputBias);
    p.rest = static_cast<float>(detail::AsymClipShape::f(static_cast<double>(p.restInput)));
}

void DriveStage::resetPath(DriveType type) noexcept
{
    Path& p = paths_[static_cast<std::size_t>(type)];
    p.pre.s = p.post.s = p.dc.s = 0.0f;
    p.cubic.reset();
    p.soft.reset();
    p.hard.reset();
    // The fuzz input at rest is G·b, not 0: prime its history there, so silence after reset() stays silent (#102).
    updateGain(type, drive_.current());
    p.asym.reset(static_cast<double>(p.restInput));
}

void DriveStage::updateLowCut(float hz) noexcept
{
    lowCutCoeffs_ = svfDesign(SvfType::highPass, sampleRate_, hz, kQ, 0.0);
}

void DriveStage::updateTone(float knob) noexcept
{
    const double dB = kToneMinDb + (kToneMaxDb - kToneMinDb) * knob / 10.0f;
    toneCoeffs_ = svfDesign(SvfType::highShelf, sampleRate_, kToneHz, kQ, dB);
}

float DriveStage::voicing(DriveType type, float x, float knob) noexcept
{
    const auto t = static_cast<std::size_t>(type);
    const Voicing& v = kVoicings[t];
    Path& p = paths_[t];
    if (knob != p.gainKnob)
        updateGain(type, knob);
    const float hp = v.preHpHz > 0.0f ? p.pre.highPass(x) : x;
    const float u = p.gain * (hp + v.inputBias) + v.fullBand * x;
    float y = 0.0f;
    switch (type) {
    case DriveType::mid:
        y = p.cubic.processSample(u);
        break;
    case DriveType::transparent:
        y = p.soft.processSample(u);
        break;
    case DriveType::dist:
        y = p.hard.processSample(u);
        break;
    case DriveType::fuzz:
        y = p.asym.processSample(u) - p.rest; // silence in, silence out
        break;
    }
    y = p.post.lowPass(v.outGain * y);
    return p.dc.highPass(y);
}

void DriveStage::process(float* io, int numSamples) noexcept
{
    if (!isPrepared()) {
        std::fill_n(io, std::max(numSamples, 0), 0.0f);
        return;
    }
    for (int n = 0; n < numSamples; ++n) {
        const float x = io[n];
        const float dry = dry1_;
        dry1_ = x;
        // Fully disabled: the smoothers stay frozen and ramp from their old values under the enable fade (harmless).
        if (bypassed_ || (!enable_.isRamping() && enable_.current() == 0.0f)) {
            io[n] = dry;
            continue;
        }
        if (!typeFade_.isRamping() && typeFrom_ != typeTo_) { // fade done: settle, then start a queued change
            typeFrom_ = typeTo_;
            typeFade_.reset(0.0f);
            if (pendingType_ != typeTo_)
                setType(pendingType_);
        }
        if (lowCut_.isSmoothing())
            updateLowCut(lowCut_.next());
        if (tone_.isSmoothing())
            updateTone(tone_.next());
        const float knob = drive_.next();

        const float tight = lowCutState_.tick(lowCutCoeffs_, x);
        float wet = voicing(typeTo_, tight, knob);
        if (typeFrom_ != typeTo_) {
            const float g = typeFade_.next();
            wet = g * wet + (1.0f - g) * voicing(typeFrom_, tight, knob);
        }
        wet = toneState_.tick(toneCoeffs_, wet);
        const float shelved = brightState_.tick(brightCoeffs_, wet);
        wet += bright_.next() * (shelved - wet);
        wet *= level_.next();
        const float out = dry + mix_.next() * (wet - dry);
        io[n] = dry + enable_.next() * (out - dry);
    }
}

} // namespace osp::dsp
