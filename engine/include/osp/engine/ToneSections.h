#pragma once

// Guitar parts and tone sections (ADR-0008 D5): a song often has more than one guitar, and a guitar often changes
// tone (clean verse, driven chorus). findSections() splits a guitar recording, ideally a Demucs guitar stem, into
// parts and, per part, into the tones it uses over time, so each can be matched to its own preset:
// "guitar 1, tone 1", "guitar 1, tone 2", "guitar 2, tone 1", ...
//
// - Parts: two parts (left, right) when the two channels carry clearly different tones (toneDistance >
//   kSplitPartsDistance) and both play; otherwise one part from pickSource(automatic), as for a double-tracked
//   guitar whose two takes share a tone.
// - Tones: each part is cut into windows (windowSeconds, hop hopSeconds); windows where that part is silent are
//   skipped; the rest are profiled and grouped by average-linkage clustering on toneDistance until the closest two
//   groups are further apart than toneThreshold. Groups shorter than minToneSeconds (transitions, fills) join the
//   nearest group. A tone that comes back later (verse, chorus, verse) is one group with several time ranges.
// Offline and non-RT.

#include "osp/engine/ToneProfile.h"

#include <string>
#include <vector>

namespace osp::engine::tonematch {

struct TimeRange {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

struct GuitarPart {
    Source source = Source::mono; // the channel the part was taken from
    std::string label;            // "left", "right", or the automatic pick
};

struct ToneSection {
    int part = 0;                  // index into SectionResult::parts
    int tone = 0;                  // per part, numbered in order of first appearance
    std::vector<TimeRange> ranges; // merged, in time order
    double seconds = 0.0;          // total length of the ranges
    Profile profile;               // of all its audio together
};

struct SectionOptions {
    double windowSeconds = 4.0;
    double hopSeconds = 2.0;
    double silenceDb = -30.0;       // a window this far below the part's loudest window is "not playing"
    double toneThreshold = 2.5;     // toneDistance above which two groups are different tones
    double minToneSeconds = 6.0;    // shorter groups are merged into the nearest one
    int maxTones = 4; // per part; the closest groups merge until there are at most this many (0 = no cap)
    bool splitParts = true; // false: always one part
    AnalysisOptions analysis;
};

// Distance between two tone profiles for grouping: the residual of their 1.25-octave smoothed spectra (so the notes
// played matter less than the tone), kFlatnessScale x the flatness distance and kSpreadScale x the envelope-spread
// distance (the gain, ToneProfile.h), combined as an RMS.
constexpr double kFlatnessScale = 1.5;
constexpr double kSpreadScale = 0.7;
constexpr double kSplitPartsDistance = 2.5;
double toneDistance(const Profile& a, const Profile& b) noexcept;

struct SectionResult {
    std::vector<GuitarPart> parts;
    std::vector<ToneSection> sections; // ordered by part, then tone
};

// right == nullptr means mono (one part). Throws std::invalid_argument when nothing is loud enough to analyse.
SectionResult findSections(
    const float* left, const float* right, std::size_t n, double sampleRate, const SectionOptions& options = {});

// The audio of one section (its ranges concatenated) from the part's source, for matching or listening.
std::vector<float> sectionAudio(const float* left,
                                const float* right,
                                std::size_t n,
                                double sampleRate,
                                const SectionResult& result,
                                const ToneSection& section);

} // namespace osp::engine::tonematch
