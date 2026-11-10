#pragma once

// One tone-match run on a worker thread (ADR-0008 A1): decodes the reference and the DI, loads the factory cab IRs,
// calls osp::engine::tonematch::runToneMatch and keeps the outcome for the message thread to read. The audio thread
// is never involved, so a run cannot cause dropouts. Control (message) thread API, except where noted.

#include "osp/engine/ToneMatchApi.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace osp::app {

struct ToneMatchSettings {
    std::filesystem::path reference;     // the song (or its guitar stem): WAV/MP3/...
    std::filesystem::path di;            // the player's dry guitar
    std::filesystem::path assetDir;      // the folder with cab/<name>.wav (the factory IRs the match chooses from)
    std::string presetName = "Tone match";
    double startSeconds = 0.0;           // the part of the reference to analyse (end <= 0: to the end)
    double endSeconds = -1.0;
    bool hpss = true;                    // drop drums; off for an isolated guitar stem
    double maxReferenceSeconds = 600.0;  // longer recordings are cut here
    double maxDiSeconds = 60.0;          // the match takes about a minute per 30 s of DI: longer DIs are cut here
};

struct ToneMatchOutcome {
    std::string presetJson; // preset.v1, empty unless the run finished
    std::vector<std::string> warnings;
    std::string error;      // why it failed, empty otherwise
    double seconds = 0.0;
};

class ToneMatchJob {
public:
    enum class State { idle, running, finished, cancelled, failed };

    ToneMatchJob() = default;
    ~ToneMatchJob(); // cancels and waits
    ToneMatchJob(const ToneMatchJob&) = delete;
    ToneMatchJob& operator=(const ToneMatchJob&) = delete;

    // False while a run is going. Starting after a finished run replaces its outcome.
    bool start(ToneMatchSettings settings);
    // Asks the run to stop at its next progress call. Any thread.
    void cancel() noexcept { cancel_.store(true); }

    State state() const noexcept { return state_.load(); }
    bool running() const noexcept { return state() == State::running; }
    // 0...1 and the name of the current step. Any thread.
    double progress() const noexcept { return progress_.load(); }
    std::string stage() const;
    // Valid once state() is finished, cancelled or failed.
    ToneMatchOutcome outcome() const;
    // Blocks until the worker is done (tests, shutdown).
    void wait();

private:
    void run(ToneMatchSettings settings);
    void finish(State state, ToneMatchOutcome outcome);

    std::thread worker_;
    std::atomic<State> state_{State::idle};
    std::atomic<bool> cancel_{false};
    std::atomic<double> progress_{0.0};
    mutable std::mutex mutex_;
    std::string stage_;
    ToneMatchOutcome outcome_;
};

} // namespace osp::app
