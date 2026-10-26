#pragma once

// Tone match (ADR-0008): estimate a chain setting that makes the player's own DI sound like the guitar in a reference
// recording (a song file, or a YouTube link resolved by tools/tonematch). Offline and non-RT: everything here
// allocates and may take seconds. No JUCE; the UI runs it on a worker thread later.
//
// Method, match-by-synthesis (every candidate is rendered with the real dsp-core blocks, so the result is a setting
// the plugin can reproduce, not an opaque filter):
// 1. Profile the reference (ToneProfile.h).
// 2. Render the DI through each drive candidate (type × drive × tight, plus clean) and profile it the same way.
// 3. Cab and EQ are linear and after the drive, so their effect on an LTAS is additive in dB: each cab IR is scored
//    from its own band response, and the 9-band graphic EQ plus the cuts are a weighted least-squares fit, refined
//    against the measured response of the real Eq block.
// 4. The best (drive, cab, EQ) by flatness distance + spectral residual is rendered end to end and re-profiled, and
//    that measured residual is what the result reports.
// Limits (also in ADR-0008): no amp block yet (Phase 5), so the drive slot is the only gain stage; a full mix still
// leaks bass, keys and vocals into the profile, which a stem (Demucs) or a solo-guitar section avoids.

#include "osp/engine/ToneProfile.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace osp::engine::tonematch {

// --- Settings (parameters.md ids) ----------------------------------------------------------------------------------

enum class DriveKind { clean, mid, transparent, dist, fuzz };
const char* toString(DriveKind k) noexcept;

struct Setting {
    float inputGainDb = 0.0f; // brings the DI's peak to kDiPeakDb
    DriveKind drive = DriveKind::clean;
    float driveKnob = 0.0f;   // 0…10
    float tightHz = 20.0f;    // drive1.lowCutHz, 20 = off
    int cab = 0;              // index into the IR list
    std::array<float, 9> eqDb{}; // eq.g63Db … g16kDb
    float eqLowCutHz = 20.0f;    // 20 = off
    float eqHighCutHz = 20000.0f; // 20000 = off
    float outputDb = 0.0f;
};

// DI nominal peak level (tone-targets-a §3.4, the same reference as Compressor::kAutoMakeupRefDb).
constexpr float kDiPeakDb = -12.0f;

struct CabIr {
    std::string name;       // shown in the report and used for factory:ir/<name>
    std::string uri;        // preset resource uri
    std::string sha256;     // lower-case hex of the WAV file, required unless uri is factory:
    std::vector<float> ir;  // conditioned to the DI's rate (IrLoader::condition)
};

// Renders `di` (already at the processing rate) through input → drive1 → cab slot 1 → EQ → output. Mono in/out.
std::vector<float> render(const std::vector<float>& di,
                          double sampleRate,
                          const Setting& s,
                          const std::vector<CabIr>& cabs,
                          int blockSize = 256);

struct MatchOptions {
    AnalysisOptions analysis;
    double flatnessWeight = 1.0; // per dB² of flatness difference
    double spectrumWeight = 0.5; // per dB² of weighted spectral residual
};

struct Candidate {
    Setting setting;
    double flatnessDistance = 0.0; // RMS dB over the flatness bands
    double residualDb = 0.0;       // robust (Huber) residual in dB, predicted (additive model)
    double score = 0.0;
};

struct MatchResult {
    Setting setting;
    Profile reference;
    Profile matched;              // profile of the final end-to-end render
    double residualDb = 0.0;      // weighted RMS spectral difference, measured on the final render
    double robustResidualDb = 0.0; // the same with Huber weights: leakage bands count less (used for the choice)
    double flatnessDistance = 0.0;
    double unmatchedResidualDb = 0.0; // the DI through the default chain (clean, cab 0, flat EQ), for comparison
    std::vector<Candidate> ranking; // the chosen setting's candidate first, then the next best by predicted score
};

// One DI, many references (ADR-0008 D5): the constructor renders and profiles the drive candidates once (the slow
// part), and each match() then costs only the cab/EQ fit and the refinement renders. Throws like match().
class Matcher {
public:
    Matcher(std::vector<float> di, double sampleRate, std::vector<CabIr> cabs, MatchOptions options = {});
    ~Matcher();
    Matcher(Matcher&&) noexcept;
    Matcher& operator=(Matcher&&) noexcept;
    MatchResult match(const Profile& reference) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

// `di` is the player's dry guitar at `sampleRate`, `reference` the profiled target, analysed at that same rate.
// Throws std::invalid_argument on empty or silent input, an empty cab list, or a reference at another rate.
MatchResult match(const Profile& reference,
                  const std::vector<float>& di,
                  double sampleRate,
                  const std::vector<CabIr>& cabs,
                  const MatchOptions& options = {});

// Preset names are at most this many characters (preset.v1.schema.json).
constexpr std::size_t kMaxNameChars = 64;
// `in` as valid UTF-8 (invalid bytes become '?', control characters a space), cut to maxChars code points.
std::string sanitiseUtf8(const std::string& in, std::size_t maxChars);

// Preset JSON (preset.v1.schema.json) for a result. `uuid` is the preset id (lower-case RFC 4122 text). The name is
// sanitised and cut to kMaxNameChars. Throws std::invalid_argument when the chosen cab is not a factory: IR and has
// no sha256.
std::string
toPresetJson(const MatchResult& r, const std::vector<CabIr>& cabs, const std::string& name, const std::string& uuid);

} // namespace osp::engine::tonematch
