#pragma once

// Public engine header: no JUCE, no NAM, no Eigen, no nlohmann (ADR-0002 REQ-NAM-009, ADR-0006 D3).
//
// One amp model behind an amp channel (ADR-0002 D2 as amended by ADR-0011 D2): a classic-DSP amp
// (makeClassicAmp, #174) or, later, a NAM capture (#169 adds loadNamModel and may only extend this header).
// ModelAmpChannel (#175) owns the models, the scratch buffers and the slot padding to D_slot.

#include "osp/rt/AudioThread.h"

#include "osp/dsp/DriveRegion.h" // dsp::OversamplingQuality

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace osp::engine {

struct AmpModelInfo { // built at load time; immutable afterwards. The audio thread never copies it.
    std::string architecture; // "classic", or the NAM architecture ("WaveNet", "LSTM", ...)
    double trainingSampleRate = 48000.0; // NAM only (ADR-0002 D4); classic models run at any rate
    std::optional<double> inputLevelDbu, outputLevelDbu, loudnessDb;
    bool supportsQuality = false; // slimmable NAM models (ADR-0002 D9)
};

class IAmpModel {
public:
    virtual ~IAmpModel() = default; // worker thread only (ADR-0004 §4)

    // Worker/control thread. May allocate. Must be called before process(). Sizes every buffer for maxBlockSize and
    // measures the model delay. `quality` picks the OS-2 factor of classic models (ADR-0005 D2); NAM ignores it
    // (its quality is a load option, ADR-0002 D9).
    virtual void prepare(double hostSampleRate, int maxBlockSize, dsp::OversamplingQuality quality) = 0;

    // Audio thread. RT-safe. Mono. `in` and `out` must not alias. 0 <= numSamples <= maxBlockSize. Before
    // prepare(), `out` is zero-filled.
    virtual void process(const float* in, float* out, int numSamples) noexcept OSP_RT_NONBLOCKING = 0;

    // Audio thread. RT-safe, O(state size), allocation-free: two renders after reset() are bit-identical.
    virtual void reset() noexcept OSP_RT_NONBLOCKING = 0;

    // Any thread, fixed after prepare(). The measured 1 kHz group delay at the host rate (ADR-0011 D2), and its
    // ceil() as an integer.
    virtual double getMeasuredDelaySamples() const noexcept = 0;
    virtual int getLatencySamples() const noexcept = 0;

    virtual const AmpModelInfo& info() const noexcept = 0;
};

// The v1 built-in models (ADR-0011 D3).
enum class ClassicAmpId { cleanA, crunchA, leadA };

// "osp.amp.clean_a", "osp.amp.crunch_a", "osp.amp.lead_a" (parameters.md §2.5).
std::string_view classicAmpModelId(ClassicAmpId id) noexcept;

// Worker thread; allocates. The concrete type has a non-virtual setParameters(const AmpParams&) (ADR-0011 D2).
std::unique_ptr<IAmpModel> makeClassicAmp(ClassicAmpId id);

} // namespace osp::engine
