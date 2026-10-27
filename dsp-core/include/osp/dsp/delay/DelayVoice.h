#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// One voice of the `delay` block (parameters.md §3.4, issue #71 part 2/4): a stereo feedback delay on two
// FractionalDelay lines (#116). The block (part 4) crossfades two voices on a mode or stereo change (xf 30) and adds
// ducking, tempo sync and trails. Part 3 adds the analog (BBD) and tape voicings to the loop.
// Loop, per side: tap = line.read(d) → low cut → high cut (TPT one-poles inside the loop, so each repeat is darker)
// → wet, then line.push(in + g·wet) with g = 0.995·feedback. The read is shortened by the high cut's DC group delay,
// 1/(2g) samples, so each repeat is centred on the set time (#71: echo time within ±1 sample) at any high cut. That is
// parameters.md §3.4's "loop gain ≤ 0.995 at 100 %": both one-poles have |H| ≤ 1, so the repeats always decay. Stereo
// (own choices inside time-based-effects §2.3):
// - mono: (L + R)/2 into one line, the same repeats on both sides.
// - pingpong: (L + R)/2 into the left line only. Left repeats feed the right line and right repeats feed the left,
//   so the repeats alternate L, R, L… one delay time apart, each g lower than the one before.
// - wide: true stereo, one line and loop per side; the right head reads 3 % later, so a mono source spreads too.
// Smoothing (parameters.md §1.4): the time is an 80 ms one-pole glide of the read pointer (tape-style: the pitch bends
// while it moves); feedback and mix are 20 ms linear ramps; the cut frequencies are 20 ms one-poles with the TPT
// coefficients updated every sample while they move. Enable is a 10 ms ramp of the wet, so disabled is exactly dry.
// The loop runs in chunks of kChunk samples: all taps of a chunk are read, then processed and pushed. A chunk is
// shorter than the 7 ms minimum delay at every rate, so no read depends on a sample of its own chunk. Part 3 needs
// this to run the saturation through a block-based oversampler inside the loop.
// Sources: J. O. Smith, PASP, "Delay Lines", https://ccrma.stanford.edu/~jos/pasp/Delay_Lines.html; V. Zavalishin,
// "The Art of VA Filter Design" 2.1.0, §3.10 (TPT one-pole),
// https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf; both accessed
// 2026-10-27.

#include "osp/dsp/util/FractionalDelay.h"
#include "osp/dsp/util/Smoothers.h"

namespace osp::dsp {

class DelayVoice {
public:
    enum class Stereo { mono, pingpong, wide };

    static constexpr float kMinMs = 7.0f, kMaxMs = 3000.0f;
    static constexpr float kMaxLoopGain = 0.995f, kWideRatio = 1.03f;
    static constexpr float kMinLowCutHz = 50.0f, kMaxLowCutHz = 800.0f, kMinHighCutHz = 500.0f,
                           kMaxHighCutHz = 16000.0f;
    static constexpr int kChunk = 32;
    // Loop state limit: never reached with |input| <= 1 and loop gain < 1; it only bounds pathological input.
    static constexpr float kLoopLimit = 8.0f;

    // Starts at the parameters.md defaults: 400 ms, feedback 30 %, mix 25 %, cuts 80 Hz / 5 kHz, mono; enabled.
    DelayVoice() noexcept;

    // Non-RT: allocates both lines for 3 s × the wide ratio.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the lines and filters and jumps every smoother to its target.
    void reset() noexcept;

    // RT-safe. Clamped to the parameters.md ranges; NaN gives the range minimum. A stereo change is a jump here: the
    // block crossfades two voices instead (xf 30).
    void setStereo(Stereo s) noexcept { stereo_ = s; }
    void setTimeMs(float ms) noexcept;
    void setFeedbackPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setLowCutHz(float hz) noexcept;
    void setHighCutHz(float hz) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input, the state is frozen.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. The wet signal only (no mix, no enable), for the block. Outputs may alias inputs. Zeros before
    // prepare().
    void processWet(const float* inL, const float* inR, float* wetL, float* wetR, int numSamples) noexcept;
    // RT-safe. In place: out = dry + mix·enable·wet. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    Stereo stereo() const noexcept { return stereo_; }
    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return lineL_.isPrepared(); }

private:
    struct OnePole { // TPT one-pole low-pass; the high-pass is x − lp
        float s = 0.0f;
        float lp(float x, float G) noexcept;
    };
    void runChunk(const float* inL, const float* inR, float* wetL, float* wetR, int n) noexcept;
    float cutCoeff(float hz) const noexcept;
    void setHighG(float G) noexcept;

    double sampleRate_ = 0.0;
    Stereo stereo_ = Stereo::mono;
    bool bypassed_ = false;
    float timeMs_ = 400.0f;
    OnePoleSmoother time_, lowCut_, highCut_; // time in samples
    LinearRamp feedback_{0.3f}, mix_{0.25f}, enabled_{1.0f};
    float lowG_ = 0.0f, highG_ = 1.0f, highDelay_ = 0.0f; // highDelay_: the high cut's DC group delay, in samples
    FractionalDelay lineL_, lineR_;
    OnePole lowL_, lowR_, highL_, highR_;
};

} // namespace osp::dsp
