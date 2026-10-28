#pragma once

// Private to engine/src/graph: the ParamStore index of every parameter the graph reads, resolved once in the
// Graph constructor (control thread), so the audio thread never looks an id up.

#include "osp/engine/Graph.h"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace osp::engine {

// Offsets into each key list, in the order of the list.
enum GateKey {
    kGateEnabled,
    kGateThreshold,
    kGateHysteresis,
    kGateAttack,
    kGateHold,
    kGateRelease,
    kGateRange,
    kGateDetector,
    kGatePostAmp
};
enum CompKey {
    kCompEnabled,
    kCompThreshold,
    kCompRatio,
    kCompKnee,
    kCompAttack,
    kCompRelease,
    kCompBlend,
    kCompMakeup,
    kCompAutoMakeup,
    kCompMode,
    kCompTone
};
enum DriveKey {
    kDriveEnabled,
    kDriveType,
    kDriveDrive,
    kDriveTone,
    kDriveLevel,
    kDriveLowCut,
    kDriveBright,
    kDriveMix
};
enum AmpKey {
    kAmpGain,
    kAmpBass,
    kAmpMid,
    kAmpTreble,
    kAmpPresence,
    kAmpDepth,
    kAmpMaster,
    kAmpLevel,
    kAmpBright,
    kAmpTight
};
enum CabKey { kCabIrEnabled, kCabIrLevel, kCabIrPan, kCabIrInvert, kCabIrDelay, kCabKeysPerIr };
enum EqKey {
    kEqMode,
    kEqGraphic9,
    kEqGraphic5 = kEqGraphic9 + 9,
    kEqParametric = kEqGraphic5 + 5,
    kEqP1Shape = kEqParametric + 12,
    kEqP4Shape,
    kEqLowCut,
    kEqHighCut,
    kEqLevel,
    kEqCount
};

enum ModKey { kModEnabled, kModType, kModRate, kModSync, kModDepth, kModMix, kModFeedback, kModWidth, kModPosition };
enum DelayKey {
    kDlyEnabled,
    kDlyMode,
    kDlyTime,
    kDlySync,
    kDlyFeedback,
    kDlyMix,
    kDlyLowCut,
    kDlyHighCut,
    kDlyStereo,
    kDlyModDepth,
    kDlySaturation,
    kDlyDuck
};
enum ReverbKey {
    kRevEnabled,
    kRevMode,
    kRevDecay,
    kRevPreDelay,
    kRevMix,
    kRevLowCut,
    kRevHighCut,
    kRevMod,
    kRevShimmer,
    kRevShimmerInterval,
    kRevFreeze
};
enum DoublerKey { kDblEnabled, kDblAmount, kDblWidth, kDblDelay, kDblDetune };

struct Graph::Ids {
    std::array<int, 2> input = resolve<2>("input.", {"enabled", "gainDb"});
    std::array<int, 9> gate = resolve<9>("gate.",
                                         {"enabled", "thresholdDb", "hysteresisDb", "attackMs", "holdMs", "releaseMs",
                                          "rangeDb", "detector", "postAmp"});
    std::array<int, 11> comp = resolve<11>("comp.",
                                           {"enabled", "thresholdDb", "ratio", "kneeDb", "attackMs", "releaseMs",
                                            "blendPct", "makeupDb", "autoMakeup", "mode", "tone"});
    std::array<std::array<int, 8>, 2> drive{driveKeys("drive1."), driveKeys("drive2.")};
    int ampChannel = find("amp.channel"), ampCleanBlend = find("amp.cleanBlendPct"), ampCabLink = find("amp.cabLink");
    std::array<std::array<int, 10>, 3> amp{ampKeys(1), ampKeys(2), ampKeys(3)};
    int cabEnabled = find("cab.enabled");
    std::array<std::array<int, 2 * kCabKeysPerIr>, 3> cab{cabKeys(1), cabKeys(2), cabKeys(3)};
    int eqEnabled = find("eq.enabled"), eqFollow = find("eq.followChannel");
    std::array<std::array<int, kEqCount>, 3> eq{eqKeys(1), eqKeys(2), eqKeys(3)};
    std::array<int, 9> mod = resolve<9>(
        "mod.", {"enabled", "type", "rateHz", "sync", "depthPct", "mixPct", "feedbackPct", "widthPct", "position"});
    int modEnabled = mod[kModEnabled], modPosition = mod[kModPosition];
    std::array<int, 12> delay = resolve<12>("delay.",
                                            {"enabled", "mode", "timeMs", "sync", "feedbackPct", "mixPct", "lowCutHz",
                                             "highCutHz", "stereo", "modDepthPct", "saturationPct", "duckDb"});
    std::array<int, 11> reverb = resolve<11>("reverb.",
                                             {"enabled", "mode", "decayS", "preDelayMs", "mixPct", "lowCutHz",
                                              "highCutHz", "modPct", "shimmerPct", "shimmerInterval", "freeze"});
    std::array<int, 5> doubler = resolve<5>("doubler.", {"enabled", "amountPct", "widthPct", "delayMs", "detuneCents"});
    std::array<int, 2> output = resolve<2>("output.", {"levelDb", "limiter"});

    static int find(const std::string& id)
    {
        const int i = findParam(id);
        if (i < 0)
            throw std::logic_error("osp::engine::Graph: no parameter " + id);
        return i;
    }

    template <std::size_t N>
    static std::array<int, N> resolve(const std::string& prefix, const std::array<std::string_view, N>& keys)
    {
        std::array<int, N> out{};
        for (std::size_t k = 0; k < N; ++k)
            out[k] = find(prefix + std::string(keys[k]));
        return out;
    }

    static std::array<int, 8> driveKeys(const std::string& prefix)
    {
        return resolve<8>(prefix, {"enabled", "type", "drive", "tone", "levelDb", "lowCutHz", "bright", "mixPct"});
    }

    static std::array<int, 10> ampKeys(int ch)
    {
        return resolve<10>("amp.ch" + std::to_string(ch) + ".", {"gain", "bass", "mid", "treble", "presence", "depth",
                                                                 "master", "levelDb", "bright", "tight"});
    }

    static std::array<int, 2 * kCabKeysPerIr> cabKeys(int ch)
    {
        const std::string p = "cab.ch" + std::to_string(ch) + ".";
        const auto a = resolve<kCabKeysPerIr>(p + "ir1.", {"enabled", "levelDb", "pan", "invert", "delayMs"});
        const auto b = resolve<kCabKeysPerIr>(p + "ir2.", {"enabled", "levelDb", "pan", "invert", "delayMs"});
        std::array<int, 2 * kCabKeysPerIr> out{};
        for (int k = 0; k < kCabKeysPerIr; ++k) {
            out[static_cast<std::size_t>(k)] = a[static_cast<std::size_t>(k)];
            out[static_cast<std::size_t>(kCabKeysPerIr + k)] = b[static_cast<std::size_t>(k)];
        }
        return out;
    }

    static std::array<int, kEqCount> eqKeys(int ch)
    {
        return resolve<kEqCount>(
            "eq.ch" + std::to_string(ch) + ".",
            {"mode",      "g63Db",     "g125Db",    "g250Db",    "g500Db",   "g1kDb",     "g2kDb",     "g4kDb",
             "g8kDb",     "g16kDb",    "m80Db",     "m240Db",    "m750Db",   "m2200Db",   "m6600Db",   "p1.freqHz",
             "p1.gainDb", "p1.q",      "p2.freqHz", "p2.gainDb", "p2.q",     "p3.freqHz", "p3.gainDb", "p3.q",
             "p4.freqHz", "p4.gainDb", "p4.q",      "p1.shape",  "p4.shape", "lowCutHz",  "highCutHz", "levelDb"});
    }
};

} // namespace osp::engine
