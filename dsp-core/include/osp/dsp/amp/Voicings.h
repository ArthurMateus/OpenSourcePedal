#pragma once

// Pure C++20: no JUCE, no OS headers (ADR-0001, ADR-0006 D3).
//
// Per-model constants of the classic amps (ADR-0011 D3, slice P5-8 / #174). Each voicing wires the merged stages,
// PreampStage (#171) -> ToneStack (#172) -> PowerAmp (#173), inside the OS-2 region; engine::ClassicAmpModel runs them.
// Starting points: docs/research/classic-amp-models.md §5 (all *unverified* there); the values below were then tuned
// by measurement against §5's THD acceptance targets (tests/engine/test_classic_amp.cpp prints them), never by ear.
// Deviations from §5 are noted on the field.
//
// Level calibration (research Q1, decided here): 0 dBFS at the amp input is kGridAt0Dbfs normalised grid units at
// the first triode (x = 1 is Vgk = 0, PreampStage.h). Reasoning: ADR-0002 D5's default interface level puts 0 dBFS at
// +12 dBu = 4.37 V peak; the default stage's bias distance Vk0 is about 1.4 V (Koren 12AX7, 1.5 kΩ, 250 V), so a
// full-scale peak is ~3 bias distances (*unverified*: it assumes the user's interface matches the default level). The
// `gain` law then models the volume pot ahead of S1. The output is trimmed per model so that a 1 kHz sine at
// −18 dBFS RMS with every knob at its default comes out at −18 dBFS RMS (outputTrimDb, measured).

#include "osp/dsp/amp/PowerAmp.h"
#include "osp/dsp/amp/PreampStage.h"
#include "osp/dsp/amp/ToneStack.h"

#include <array>

namespace osp::dsp::amp {

inline constexpr double kGridAt0Dbfs = 3.0;
inline constexpr int kMaxPreampStages = 4;

enum class AmpVoicingId { cleanA, crunchA, leadA };
inline constexpr int kNumAmpVoicings = 3;

struct PreampStageVoicing {
    PreampConfig config{};
    float couplingHz = 12.0f, millerHz = 20000.0f, cathodeHz = 200.0f;
    float driveDb = 0.0f; // fixed drive of stages after the first (S1's drive is the gain law)
};

struct AmpVoicing {
    const char* id; // parameters.md §2.5 model id
    int numStages; // 1…kMaxPreampStages
    int toneStackAfter; // the tone stack follows this many preamp stages (1…numStages)
    std::array<PreampStageVoicing, kMaxPreampStages> stages;
    float gainDbAt0, gainDbPerKnob; // S1 drive = gainDbAt0 + gain·gainDbPerKnob (dB, clamped to ±40 by the stage)
    ToneStackVariant stack;
    double stackMakeupDb; // after the tone stack: its insertion loss is recovered by the next stage's grid
    PowerAmpVoicing power; // sagAmount stays 0 (#173 patent note, owner review pending)
    double powerInputDb; // preamp output -> power-amp input
    double brightDb, brightHz; // bright shelf at gain 0, fading linearly to 0 dB at gain 10
    double tightHz; // tight: a 1st-order HPF ahead of S1 (on top of S1's coupling HPF)
    float tightCathodeHz; // tight: added to every cathode-shelf corner (0 = unchanged)
    double outputTrimDb; // measured: −18 dBFS RMS in -> −18 dBFS RMS out at the defaults
};

// clang-format off
inline constexpr std::array<AmpVoicing, kNumAmpVoicings> kAmpVoicings{{
    // Hi-fi clean, big headroom: S1 -> TS -> S2 -> PA.
    {"osp.amp.clean_a", 2, 1,
     {{{PreampConfig{.cathodeShelfDb = 0.0, .outputGainDb = 0.0}, 12.0f, 20000.0f, 200.0f, 0.0f},
       {PreampConfig{.cathodeShelfDb = 0.0, .outputGainDb = 0.0}, 12.0f, 20000.0f, 200.0f, 0.0f}, {}, {}}},
     -30.6f, 3.0f, ToneStackVariant::a, 6.0, // §5: −24 + 4.8·g, +16 dB makeup; see the design note
     PowerAmpVoicing{.shape = PowerAmpShape::beam, .nfbDb = 14.0, .ceiling = 1.0, .masterDbAt5 = 0.0,
                     .masterDbPerKnob = 2.0, .loopHpHz = 4.0, .otHpHz = 40.0, .otLpHz = 14000.0, .otLpQ = 0.8},
     14.0, 6.0, 2500.0, 60.0, 0.0f, 10.59},
    // Edge of breakup to master-volume crunch: S1 -> S2 -> TS -> PA.
    {"osp.amp.crunch_a", 2, 2,
     {{{PreampConfig{.cathodeShelfDb = 0.0, .outputGainDb = 0.0}, 20.0f, 16000.0f, 200.0f, 0.0f},
       {PreampConfig{.cathodeShelfDb = -4.0, .outputGainDb = 0.0}, 20.0f, 16000.0f, 100.0f, 0.0f}, {}, {}}},
     -18.0f, 4.2f, ToneStackVariant::b, 0.0, // §5: −12 + 4.2·g
     PowerAmpVoicing{.shape = PowerAmpShape::pentode, .nfbDb = 12.0, .ceiling = 1.0, .masterDbAt5 = 0.0,
                     .masterDbPerKnob = 3.0, .loopHpHz = 4.0, .otHpHz = 50.0, .otLpHz = 11000.0, .otLpQ = 0.8},
     0.0, 5.0, 2000.0, 90.0, 0.0f, -3.06},
    // High-gain lead, tight low end: S1 -> S2 -> TS -> S3 -> S4 -> PA (research Q11: tone stack mid-preamp).
    {"osp.amp.lead_a", 4, 2,
     {{{PreampConfig{.cathodeShelfDb = 0.0, .outputGainDb = 0.0}, 25.0f, 16000.0f, 200.0f, 0.0f},
       {PreampConfig{.cathodeShelfDb = -6.0, .outputGainDb = 0.0}, 70.0f, 12000.0f, 200.0f, 0.0f},
       {PreampConfig{.cathodeShelfDb = -8.0, .outputGainDb = 0.0}, 90.0f, 10000.0f, 250.0f, 0.0f},
       {PreampConfig{.cathodeShelfDb = -10.0, .outputGainDb = 0.0}, 150.0f, 8000.0f, 300.0f, 0.0f}}},
     0.0f, 4.0f, ToneStackVariant::b, 0.0, // §5 says 0 + 4.5·g; the stage clamps drive at +40 dB, so 4.0·g
     PowerAmpVoicing{.shape = PowerAmpShape::pentode, .nfbDb = 16.0, .ceiling = 1.0, .masterDbAt5 = 0.0,
                     .masterDbPerKnob = 3.0, .loopHpHz = 4.0, .otHpHz = 60.0, .otLpHz = 9000.0, .otLpQ = 0.8},
     0.0, 4.0, 3000.0, 150.0, 100.0f, -6.13},
}};
// clang-format on

inline const AmpVoicing& ampVoicing(AmpVoicingId id) noexcept
{
    return kAmpVoicings[static_cast<std::size_t>(id)];
}

} // namespace osp::dsp::amp
