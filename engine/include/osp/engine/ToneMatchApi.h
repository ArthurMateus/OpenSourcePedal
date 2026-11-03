#pragma once

// Tone match for the app (ADR-0008 amendment A1, #197): the reference recording's audio buffer + the player's DI
// buffer in, a preset document out. Offline, allocates and takes seconds to minutes (about a minute for a 30 s DI on
// 8 cores), so call it from a worker thread, never the audio or message thread. No JUCE, no file or network I/O: the
// caller decodes the audio (WAV/MP3/YouTube are the front end's job, see tools/tonematch) and loads the cab IRs.
//
// Threads: runToneMatch uses up to ToneMatchRequest::threads internal worker threads and calls `progress` from any of
// them, one call at a time; make the callback cheap and thread-safe (post to the UI thread, set an atomic).

#include "osp/engine/ToneMatch.h"

#include <functional>
#include <string>
#include <vector>

namespace osp::engine::tonematch {

struct AudioBuffer {
    std::vector<float> left;  // mono signal, or the left channel
    std::vector<float> right; // empty = mono; otherwise the same length as `left`
    double sampleRate = 0.0;
};

// Returns false to cancel. `fraction` is 0...1 over the whole run and never decreases; `stage` names the step
// ("profile", "candidates", "refine", "preset", ...), valid only during the call.
using ProgressFn = std::function<bool(double fraction, const char* stage)>;

struct ToneMatchRequest {
    AudioBuffer reference; // the song, or better its guitar stem (Demucs)
    AudioBuffer di;        // the player's dry guitar: the left channel is used
    // Cab IRs conditioned to the DI's sample rate (IrLoader::condition); the preset refers to each by `uri`
    // (`factory:ir/<name>`, or a file: uri plus sha256, see toPresetJson).
    std::vector<CabIr> cabs;
    Source source = Source::automatic; // which channel of a stereo reference to analyse
    double startSeconds = 0.0;         // analyse only this part of the reference (0 / negative end = whole)
    double endSeconds = -1.0;
    bool hpss = true;      // drop drums; off for an isolated guitar stem
    bool searchAmps = true; // pick the classic amp (model, gain, tone knobs); off = the pedal + cab + EQ search of D3
    std::string presetName = "Tone match";
    std::string presetId;   // RFC 4122 lower-case text; empty = a random one
    int threads = 0;        // 0 = hardware concurrency
};

struct ToneMatchResponse {
    bool cancelled = false;
    MatchResult match;           // the chosen setting, the reference/matched profiles and the distances
    Source sourceUsed = Source::mono;
    std::string presetJson;      // preset.v1 document, empty when cancelled
    std::vector<std::string> warnings; // plain-language caveats for the UI: dark DI, EQ at its limit, ...
    double seconds = 0.0;        // wall time of the run
};

// Throws std::invalid_argument for an empty/silent DI, no cabs, a bad sample rate or a bad time range. Cancelling is
// not an error: the response has cancelled = true.
ToneMatchResponse runToneMatch(const ToneMatchRequest& request, const ProgressFn& progress = {});

} // namespace osp::engine::tonematch
