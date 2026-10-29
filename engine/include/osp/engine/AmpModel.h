#pragma once

// Public engine header: no JUCE, no NAM, no Eigen, no nlohmann (ADR-0002 REQ-NAM-009, ADR-0006 D3).
//
// One amp model behind an amp channel (ADR-0002 D2 as amended by ADR-0011 D2): a classic-DSP amp
// (makeClassicAmp, #174) or a NAM capture (loadNamModel, #169).
// ModelAmpChannel (#175) owns the models, the scratch buffers and the slot padding to D_slot.

#include "osp/rt/AudioThread.h"

#include "osp/dsp/DriveRegion.h" // dsp::OversamplingQuality

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
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

// NAM load options (ADR-0002 D9): applied by the worker at load time, never on the audio thread.
struct AmpLoadOptions {
    float quality01 = 1.0f; // slimmable (A2) models: network size, 0..1 ("Amp CPU quality"); ignored otherwise
};

// Thrown by loadNamModel() and by a NAM model's prepare(); what() is a user-readable message.
class ModelLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Worker thread; allocates and may throw ModelLoadError (ADR-0002 D2, D3). Accepts .nam versions 0.5.0-0.7.x as
// NAM core's version checker does, mono (1 in, 1 out) only. The model must be prepare()d at its training rate (48 kHz
// when the file has none); other host rates throw ModelLoadError until resampling lands (#170).
std::unique_ptr<IAmpModel> loadNamModel(const std::filesystem::path& path, const AmpLoadOptions& options = {});

} // namespace osp::engine
