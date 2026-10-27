#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// One voice of the `delay` block (parameters.md §3.4, issue #71 parts 2–3): a stereo feedback delay on two
// FractionalDelay lines (#116), with the digital, analog (BBD) and tape voicings of `delay.mode`. The block (part 4)
// crossfades two voices on a mode or stereo change (xf 30) and adds ducking, tempo sync and trails.
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
// shorter than the 7 ms minimum delay at every rate, so no read depends on a sample of its own chunk, and the
// analog/tape saturation can run through the block-based oversampler inside the loop (LoopSaturator).
// Voicings (part 3; time-based-effects §2.3, constants are own choices inside its ranges):
// - analog (BBD): tap → 2nd-order Butterworth low-pass at the BBD's clock limit, fc = 0.45·N/(2T) for N = 4096
//   stages, clamped to 1.8–3.5 kHz → asymmetric saturation (β = 0.2) → the cuts. The clock wobbles with a 0.7 Hz
//   triangle of ±min(0.5 % of T, 1.5 ms)·depth. After C. Raffel, J. O. Smith, "Practical Modeling of Bucket-Brigade
//   Device Circuits", DAFx-10, https://dafx10.iem.at/papers/RaffelSmith_DAFx10_P42.pdf (accessed 2026-10-27).
// - tape: tap → 4.5 kHz Butterworth low-pass (head HF loss) → tanh saturation → the cuts; wow is a 0.8 Hz sine of
//   ±min(0.4 % of T, 2 ms)·depth and flutter a 6.5 Hz sine of ±0.03 ms·depth; an 80 Hz +3 dB (Q 1.2) head bump on
//   the wet output only, so it never lifts the loop gain. After S. Arnardottir, J. S. Abel, J. O. Smith, tape-delay
//   model, AES 125th Convention, paper 7649, 2008, https://aes2.org/publications/elibrary-page/?id=14800 (abstract;
//   accessed 2026-10-27), and time-based-effects §2.3.
// - Saturation g = 0.05 + 3.95·s² (s = saturationPct/100; 20 % gives a light g = 0.21). Its slope is <= 1 everywhere
//   (LoopSaturator), and the BBD's bias costs 1 − tanh²(0.2) = −0.34 dB per repeat; the low-passes have |H| <= 1. So
//   the loop gain stays <= 0.995 in every voicing.
// - The modulation depth and saturation are 20 ms one-poles (parameters.md §1.4). Digital ignores both.
// Sources: J. O. Smith, PASP, "Delay Lines", https://ccrma.stanford.edu/~jos/pasp/Delay_Lines.html; V. Zavalishin,
// "The Art of VA Filter Design" 2.1.0, §3.10 (TPT one-pole),
// https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf; both accessed
// 2026-10-27.

#include "osp/dsp/delay/LoopSaturator.h"
#include "osp/dsp/util/FractionalDelay.h"
#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

namespace osp::dsp {

class DelayVoice {
public:
    enum class Voicing { digital, analog, tape };
    enum class Stereo { mono, pingpong, wide };

    static constexpr float kMinMs = 7.0f, kMaxMs = 3000.0f;
    static constexpr float kMaxLoopGain = 0.995f, kWideRatio = 1.03f;
    static constexpr float kMinLowCutHz = 50.0f, kMaxLowCutHz = 800.0f, kMinHighCutHz = 500.0f,
                           kMaxHighCutHz = 16000.0f;
    static constexpr int kChunk = 32;
    static constexpr float kBbdStages = 4096.0f, kBbdMinHz = 1800.0f, kBbdMaxHz = 3500.0f, kBbdBias = 0.2f;
    static constexpr float kTapeLowPassHz = 4500.0f, kMaxModMs = 2.0f;
    // Loop state limit: never reached with |input| <= 1 and loop gain < 1; it only bounds pathological input.
    static constexpr float kLoopLimit = 8.0f;

    // Starts at the parameters.md defaults: digital, 400 ms, feedback 30 %, mix 25 %, cuts 80 Hz / 5 kHz, mono,
    // modulation 0 %, saturation 20 %; enabled.
    DelayVoice() noexcept;

    // Non-RT: allocates both lines for 3 s × the wide ratio plus the modulation, and the loop oversamplers.
    void prepare(double sampleRate, int maxBlockSize);
    // RT-safe. Clears the lines and filters and jumps every smoother to its target.
    void reset() noexcept;
    // RT-safe (no allocation: both voices are prepared alike). Takes over `other`'s lines, filters, smoothers and
    // parameters, so the block can start a crossfade to a new mode or stereo setting with the repeats intact. The loop
    // oversamplers restart from silence (they cannot be copied); the crossfade covers that.
    void copyStateFrom(const DelayVoice& other) noexcept;

    // RT-safe. Clamped to the parameters.md ranges; NaN gives the range minimum. A stereo change is a jump here: the
    // block crossfades two voices instead (xf 30).
    void setVoicing(Voicing v) noexcept { voicing_ = v; }
    void setStereo(Stereo s) noexcept { stereo_ = s; }
    void setTimeMs(float ms) noexcept;
    void setFeedbackPct(float pct) noexcept;
    void setMixPct(float pct) noexcept;
    void setLowCutHz(float hz) noexcept;
    void setHighCutHz(float hz) noexcept;
    void setModDepthPct(float pct) noexcept;
    void setSaturationPct(float pct) noexcept;
    void setEnabled(bool on) noexcept { enabled_.setTarget(on ? 1.0f : 0.0f); }
    // Hard bypass for tests (signal-flow §5): output = input, the state is frozen.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. The wet signal only (no mix, no enable), for the block. Outputs may alias inputs. Zeros before
    // prepare().
    void processWet(const float* inL, const float* inR, float* wetL, float* wetR, int numSamples) noexcept;
    // RT-safe. In place: out = dry + mix·enable·wet. Before prepare() it zero-fills.
    void process(float* left, float* right, int numSamples) noexcept;

    Voicing voicing() const noexcept { return voicing_; }
    Stereo stereo() const noexcept { return stereo_; }
    // The loop's oversampling factor for the saturating voicings: 2 below 88.2 kHz, else 1 (signal-flow §3).
    int getLoopOversampling() const noexcept { return satL_.factor(); }
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
    float modOffset(float d) noexcept;

    double sampleRate_ = 0.0;
    Voicing voicing_ = Voicing::digital;
    Stereo stereo_ = Stereo::mono;
    bool bypassed_ = false;
    float timeMs_ = 400.0f;
    OnePoleSmoother time_, lowCut_, highCut_, modDepth_, saturation_; // time in samples
    LinearRamp feedback_{0.3f}, mix_{0.25f}, enabled_{1.0f};
    float lowG_ = 0.0f, highG_ = 1.0f, highDelay_ = 0.0f; // highDelay_: the high cut's DC group delay, in samples
    FractionalDelay lineL_, lineR_;
    OnePole lowL_, lowR_, highL_, highR_;
    LoopSaturator satL_, satR_;
    SvfState colourL_, colourR_, bumpL_, bumpR_;
    SvfCoeffs colour_, bump_;
    float colourHz_ = 0.0f, colourDelay_ = 0.0f; // the voicing low-pass and its DC group delay in samples
    double lfoPhase_ = 0.0, flutterPhase_ = 0.0;
};

} // namespace osp::dsp
