#pragma once

// Public engine header: no JUCE, no NAM, no Eigen (ADR-0006 D3).
//
// The v1 parameter table (docs/spec/parameters.md, ADR-0009 D8) and the store the graph reads once per block.
// Values are plain (unit) values: dB, ms, Hz, %, knob. A choice holds its index (0…n−1) and a bool holds 0 or 1.
// Resources (amp.ch<N>.model, cab.ch<N>.ir1/ir2) are objects installed through the ADR-0004 path, not parameters.
//
// Own decision (parameters.md lists "off, 1/1 … 1/16 (+ D/T variants)" for mod.sync): the order is off, the five
// straight values long to short, then the dotted ones, then the triplets. delay.sync follows dsp::SyncDivision.

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osp::engine {

enum class Skew : unsigned char { lin, log, step };
enum class ParamKind : unsigned char { continuous, boolean, choice, integer };
// host = automatable and in the VST3 list (H); preset = preset-only (P); reserved = post-v1, kept in presets only.
enum class Visibility : unsigned char { host, preset, reserved };

struct ParamInfo {
    std::string id; // full id, e.g. "amp.ch2.gain"
    float min = 0.0f, max = 1.0f, def = 0.0f;
    Skew skew = Skew::lin;
    ParamKind kind = ParamKind::continuous;
    Visibility vis = Visibility::host;
    std::string_view unit; // "dB", "ms", "s", "Hz", "%", "knob", "ratio", "st", "cents", or empty
    std::span<const std::string_view> choices; // the choice names, in index order; empty otherwise
};

// Control thread. Built on first use (allocates once), immutable afterwards. The order is stable within a build and
// is the ParamStore index; it is not a file format (presets and host state use the ids).
std::span<const ParamInfo> paramTable();
// Control thread. Index of `id`, or -1.
int findParam(std::string_view id);
// Number of host-visible entries (parameters.md §5: 226).
int hostParamCount();

// Clamps to [min, max] (NaN → min) and snaps bool, choice and integer values. RT-safe.
float sanitize(const ParamInfo& p, float plain) noexcept;
// The 0…1 host value for a plain value and back, with the table's skew (parameters.md §1.3). RT-safe.
float toNormalized(const ParamInfo& p, float plain) noexcept;
float fromNormalized(const ParamInfo& p, float normalized) noexcept;

static_assert(std::atomic<float>::is_always_lock_free); // realtime-rules RT-6

// One value per table entry, at the defaults. The audio thread reads every entry once per block (relaxed,
// threading.md §3); an entry may be bound to an external atomic (the host's parameter value).
class ParamStore {
public:
    ParamStore();

    int size() const noexcept { return size_; }

    // Control thread. Writes the store's own value (sanitized). A bound entry keeps reading its external atomic.
    void set(int index, float plain) noexcept;
    // Control thread. False for an unknown id.
    bool set(std::string_view id, float plain);
    // Control thread, audio stopped. nullptr unbinds. The atomic must outlive the binding.
    void bind(int index, const std::atomic<float>* external) noexcept;

    // RT-safe. The raw value (bound or own); the graph sanitizes it. 0 for an index out of range.
    float get(int index) const noexcept
    {
        return index >= 0 && index < size_ ? src_[static_cast<std::size_t>(index)]->load(std::memory_order_relaxed)
                                           : 0.0f;
    }

private:
    int size_ = 0;
    std::unique_ptr<std::atomic<float>[]> own_;
    std::vector<const std::atomic<float>*> src_;
};

} // namespace osp::engine
