#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// The `mod` block (parameters.md §3.3, signal-flow §1, issue #70, part 4/4): one modulation effect per preset, typed by
// `mod.type`, at the `pre` (after Drive B, before the amp, mono) or `post` (after the EQ and clean blend, stereo)
// position. Zero latency in both (signal-flow §4, L_mod = 0).
// - Engines: ChorusFlanger (chorus80s, chorusWide, flanger), Phaser (phaser, vibe) and Tremolo (tremolo).
// - Type change (`xf 30`, parameters.md §1.4): two slots, each holding all three engines. The idle slot is set to the
//   new type and reset(), then a 30 ms constant-amplitude crossfade (util/Crossfader.h) runs from the old slot to it;
//   both run in parallel only during the fade. A change that arrives mid-fade is applied when that fade ends.
// - Position: processStereo() for `post`; processMono() for `pre`, which runs the stereo engine on a copy of the mono
//   input and averages the two sides ("stereo voices are summed", signal-flow §1). The position is preset-only: the
//   engine moves the block and crossfades instances on a change (ADR-0004 §5); this class only records it.
// - Sync (`mod.sync`): with a tempo, rate = bpm / 60 / beats, clamped to the 0.1…10 Hz range; beats 0 = `off`.
//   syncBeats() maps the parameters.md values (1/1 = 4 beats, D = ×1.5, T = ×2/3).
// - Enable/bypass: `mod.enabled` forwards to the engines (10 ms ramp of their mix, so disabled is exact dry);
//   setBypassed() is the hard bypass (signal-flow §5).

#include "osp/dsp/ChorusFlanger.h"
#include "osp/dsp/Phaser.h"
#include "osp/dsp/Tremolo.h"
#include "osp/dsp/util/Crossfader.h"

#include <array>
#include <string_view>
#include <vector>

namespace osp::dsp {

class Modulation {
public:
    enum class Type { chorus80s, chorusWide, flanger, phaser, vibe, tremolo };
    enum class Position { post, pre };
    static constexpr double kTypeFadeSeconds = 0.030;

    // Beats per LFO cycle for a `mod.sync` value ("off", "1/1" … "1/16", with a "D" or "T" suffix); 0 = off or unknown.
    static float syncBeats(std::string_view value) noexcept;

    // Starts at the parameters.md defaults: chorusWide, 0.8 Hz, sync off, depth 50 %, mix 50 %, feedback 0, width 100
    // %, post; enabled.
    Modulation() noexcept;

    // Non-RT: allocates the engines' rings and the scratch buffers for maxBlockSize.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Resets both slots, settles on the current type and drops any pending change.
    void reset() noexcept;

    // RT-safe, once per block. Ranges and NaN handling as the engines (parameters.md §3.3).
    void setType(Type type) noexcept;
    void setRateHz(float hz) noexcept;
    void setSyncBeats(float beats) noexcept;
    void setTempoBpm(float bpm) noexcept;
    void setDepthPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setFeedbackPct(float pct) noexcept;
    void setWidthPct(float pct) noexcept;
    void setEnabled(bool on) noexcept;
    void setPosition(Position p) noexcept { position_ = p; }
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place, any numSamples >= 0 (processed in chunks of maxBlockSize). Before prepare() they zero-fill.
    void processStereo(float* left, float* right, int numSamples) noexcept;
    void processMono(float* samples, int numSamples) noexcept;

    Type type() const noexcept { return pending_ ? pendingType_ : slots_[static_cast<std::size_t>(current())].type; }
    Position position() const noexcept { return position_; }
    float effectiveRateHz() const noexcept;
    bool isFading() const noexcept { return fadeL_.isFading(); }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return maxBlock_ > 0; }

private:
    struct Slot {
        Type type = Type::chorusWide;
        ChorusFlanger chorus;
        Phaser phaser;
        Tremolo tremolo;
        void setType(Type t) noexcept;
        void process(float* l, float* r, int n) noexcept;
    };
    template <typename F>
    void forEachEngine(F&& f) noexcept;
    int current() const noexcept { return fadeL_.target() ? 1 : 0; }
    void startFade(Type type) noexcept;
    void applyRate() noexcept;
    void processChunk(float* left, float* right, int n) noexcept;

    std::array<Slot, 2> slots_;
    Crossfader fadeL_, fadeR_;
    bool pending_ = false, bypassed_ = false;
    Type pendingType_ = Type::chorusWide;
    Position position_ = Position::post;
    float rateHz_ = 0.8f, syncBeats_ = 0.0f, bpm_ = 0.0f;
    int maxBlock_ = 0;
    std::vector<float> bufL_, bufR_, monoR_;
};

} // namespace osp::dsp
