#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// The `delay` block (parameters.md §3.4, issue #71 part 4/4): stereo in, stereo out, zero latency (signal-flow §4,
// L_dly = 0). It wraps the DelayVoice of parts 2–3 and adds:
// - Mode and stereo changes (`xf 30`): two voice slots. On a change the idle slot takes over the active slot's state
//   (DelayVoice::copyStateFrom, so the repeats carry on), switches to the new mode/stereo, and a 30 ms
//   constant-amplitude (linear) crossfade runs from the old slot to it (signal-flow §5). Both slots run only during the
//   fade; a change that arrives mid-fade is applied when the fade ends.
// - Tempo sync (`delay.sync`): with a division and a valid host tempo, the time is TempoSync::timeMs(division, bpm)
//   (#116), otherwise `delay.timeMs`. Either way the voices glide to it over 80 ms (parameters.md §1.4).
// - Ducking (`delay.duckDb`): a peak follower on the dry input, 5 ms attack / 200 ms release (time-based-effects §2.3),
//   turns the wet down by duckDb·clamp((env_dB + 42)/12, 0, 1): nothing below −42 dBFS, the full depth from −30 dBFS
//   up (own choice of knee). The repeats swell back between phrases. duckDb is a 20 ms one-pole.
// - Mix: out = dry + mix·duck·wet, the dry at unity (time-based-effects §3.2). Mix is a 20 ms linear ramp.
// - Enable (`delay.enabled`): a 10 ms ramp of the wet (signal-flow §5). Once settled off, the voices stop and the
//   output is exactly the dry; they restart from silence when enabled again.
// - Soft bypass (trails, signal-flow §5): setSoftBypass(true) ramps the voices' input send to 0 over 10 ms and lets
//   the repeats ring out. setBypassed(true) is the hard bypass for tests: output = input.
// Calls longer than maxBlockSize are processed in chunks.

#include "osp/dsp/delay/DelayVoice.h"
#include "osp/dsp/delay/TempoSync.h"
#include "osp/dsp/util/Smoothers.h"

#include <vector>

namespace osp::dsp {

class Delay {
public:
    using Mode = DelayVoice::Voicing;
    using Stereo = DelayVoice::Stereo;
    static constexpr double kTypeFadeSeconds = 0.030;
    static constexpr float kMaxDuckDb = 24.0f, kDuckFloorDb = -42.0f, kDuckKneeDb = 12.0f;

    // Starts at the parameters.md defaults (digital, 400 ms, sync off, feedback 30 %, mix 25 %, 80 Hz / 5 kHz, mono,
    // modulation 0, saturation 20 %, ducking 0 dB); enabled.
    Delay() noexcept;

    // Non-RT: prepares both slots and the chunk buffers.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Settles any pending mode/stereo change, clears both slots and the ducker.
    void reset() noexcept;

    // RT-safe. Values are clamped as in DelayVoice; NaN gives the range minimum.
    void setMode(Mode m) noexcept;
    void setStereo(Stereo s) noexcept;
    void setTimeMs(float ms) noexcept;
    void setSync(SyncDivision d) noexcept;
    void setTempoBpm(double bpm) noexcept;
    void setFeedbackPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setLowCutHz(float hz) noexcept;
    void setHighCutHz(float hz) noexcept;
    void setModDepthPct(float pct) noexcept;
    void setSaturationPct(float pct) noexcept;
    void setDuckDb(float db) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    void setSoftBypass(bool on) noexcept { send_.setTarget(on ? 0.0f : 1.0f); }
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on both channels. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    Mode mode() const noexcept { return mode_; }
    Stereo stereo() const noexcept { return stereo_; }
    // The time the voices glide to, in ms (the synced time when sync applies).
    float effectiveTimeMs() const noexcept { return effectiveMs_; }
    bool isCrossfading() const noexcept { return fade_.isRamping(); }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return maxBlock_ > 0; }

private:
    template <typename Fn>
    void forEachSlot(Fn&& fn) noexcept
    {
        fn(slots_[0]);
        fn(slots_[1]);
    }
    void updateTime() noexcept;
    void startSwitchIfNeeded() noexcept;
    int activeSlot() const noexcept { return fade_.target() > 0.5f ? 1 : 0; }
    void processChunk(float* left, float* right, int n) noexcept;

    DelayVoice slots_[2];
    LinearRamp fade_{0.0f}; // 0 = slot 0, 1 = slot 1
    Mode mode_ = Mode::digital;
    Stereo stereo_ = Stereo::mono;
    float timeMs_ = 400.0f, effectiveMs_ = 400.0f;
    SyncDivision sync_ = SyncDivision::off;
    double bpm_ = 0.0;
    LinearRamp mix_{0.25f}, enabled_{1.0f}, send_{1.0f};
    OnePoleSmoother duckDb_;
    float env_ = 0.0f, attack_ = 0.0f, release_ = 0.0f;
    bool bypassed_ = false, voicesIdle_ = false;
    int maxBlock_ = 0;
    std::vector<float> sendL_, sendR_, wetL_[2], wetR_[2];
};

} // namespace osp::dsp
