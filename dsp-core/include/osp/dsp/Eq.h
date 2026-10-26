#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// EQ block, `eq.ch<N>.*` in parameters.md §3.2: one block, three voicings (graphic9, graphic5, parametric) plus a
// 2nd-order low cut and high cut and an output level. Mono or linked stereo (signal-flow §2: EQ → Output is
// stereo). Latency 0 (signal-flow §4: minimum-phase). One instance holds one amp channel's settings; the engine
// keeps one instance per amp channel (`perChannel`) and crossfades between them on a channel switch (ADR-0004).
//
// Filters: TPT state-variable filters (util/Svf.h, Simper 2013). Graphic bands are bells with a fixed Q (1.0 for
// the octave bands, 0.7 for the 5-band voicing, parameters.md §3.2). Parametric p1/p4 are a bell or a low/high
// shelf. The cuts are Butterworth (Q = 1/sqrt 2).
//
// Smoothing (parameters.md §1.4): band gain (dB), log-frequency and log-Q are one-pole 20 ms, and a band's
// coefficients are recomputed every sample while any of them moves. The level is a 20 ms linear ramp of the linear
// gain. Mode and edge-shape switches crossfade over 20 ms (xf 20): the incoming voicing starts from a cleared state
// and both run during the fade. Only the active voicing processes audio otherwise.
//
// Decision: a cut at its range end (low cut 20 Hz, high cut 20 kHz) is off, so the defaults are an exact identity
// (a 20 kHz 2nd-order low pass would take about 1.5 dB off the 16 kHz band at 44.1 kHz). Leaving or reaching the
// end crossfades the cut in or out over 20 ms; the cut filter keeps running, so its state is always warm.

#include "osp/dsp/util/Smoothers.h"
#include "osp/dsp/util/Svf.h"

#include <array>

namespace osp::dsp {

enum class EqMode { graphic9, graphic5, parametric };
enum class EqEdgeShape { peak, shelf };

class Eq {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr std::array<double, 9> kGraphic9Hz{63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    static constexpr std::array<double, 5> kGraphic5Hz{80, 240, 750, 2200, 6600};
    static constexpr double kGraphic9Q = 1.0;
    static constexpr double kGraphic5Q = 0.7;
    static constexpr double kCutQ = 0.70710678118654752;

    // Starts at the parameters.md defaults.
    Eq() noexcept;

    // Non-RT. No allocation either.
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    // RT-safe. Clears every filter state and jumps every smoother to its target: two renders after reset() match.
    void reset() noexcept;

    // RT-safe, once per block. Values are clamped to the parameters.md ranges (NaN maps to the range minimum).
    void setMode(EqMode mode) noexcept;
    void setGraphic9Db(int band, float dB) noexcept; // band 0...8 = 63 Hz ... 16 kHz
    void setGraphic5Db(int band, float dB) noexcept; // band 0...4 = 80 Hz ... 6.6 kHz
    void setParametric(int band, float hz, float gainDb, float q) noexcept; // band 0...3 = p1...p4
    void setEdgeShape(int band, EqEdgeShape shape) noexcept;                // band 0 (p1) or 3 (p4)
    void setLowCutHz(float hz) noexcept;
    void setHighCutHz(float hz) noexcept;
    void setLevelDb(float dB) noexcept;
    // Hard bypass for tests (signal-flow §5): output = input.
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }

    // RT-safe. In place on 1 or 2 channels (more are left untouched). Before prepare() it zero-fills.
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    bool isPrepared() const noexcept { return sampleRate_ > 0.0; }

private:
    static constexpr int kChunk = 64;
    using Chunk = float[kMaxChannels][kChunk];

    struct Band {
        SvfType type = SvfType::bell;
        OnePoleSmoother logHz, gainDb, logQ;
        SvfCoeffs c;
        SvfState s[kMaxChannels];
        bool dirty = true;

        void set(double hz, float dB, double q) noexcept;
        void restart() noexcept; // cold start: states cleared, smoothers at their targets
        void process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept;
    };

    struct Edge {
        Band peak, shelf;
        LinearRamp toShelf; // 0 = peak, 1 = shelf, 20 ms (xf 20)
        void process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept;
    };

    struct Cut {
        Band filter;
        LinearRamp on; // 0 = off, 1 = on, 20 ms
        void process(Chunk& buf, int numChannels, int n, double sampleRate) noexcept;
    };

    void restartVoicing(EqMode mode) noexcept;
    void processVoicing(EqMode mode, Chunk& buf, int numChannels, int n) noexcept;
    void processChunk(float* const* channels, int numChannels, int offset, int n) noexcept;

    double sampleRate_ = 0.0;
    bool bypassed_ = false;
    EqMode modeFrom_ = EqMode::graphic9, modeTo_ = EqMode::graphic9, pendingMode_ = EqMode::graphic9;
    LinearRamp modeFade_; // 0 = modeFrom_, 1 = modeTo_, 20 ms (xf 20)
    std::array<Band, 9> graphic9_;
    std::array<Band, 5> graphic5_;
    Edge p1_, p4_;
    Band p2_, p3_;
    Cut lowCut_, highCut_;
    LinearRamp level_; // linear gain, 20 ms
};

} // namespace osp::dsp
