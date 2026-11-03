#include "BlockControls.h"

#include "BlockInfo.h"

#include <array>

namespace osp::ui {

namespace {
using enum ControlKind;

// Every key below exists in the v1 host table (parameters.md §2, §3). Preset-only options (gate detector, EQ mode,
// modulation position, ...) are not host parameters and have no control here.
constexpr std::array kInput{ControlSpec{knob, "gainDb", "Input"}};
constexpr std::array kGate{
    ControlSpec{knob, "thresholdDb", "Threshold"}, ControlSpec{knob, "rangeDb", "Range"},
    ControlSpec{knob, "attackMs", "Attack"},       ControlSpec{knob, "holdMs", "Hold"},
    ControlSpec{knob, "releaseMs", "Release"},     ControlSpec{knob, "hysteresisDb", "Hysteresis"},
    ControlSpec{toggle, "postAmp", "Post-amp"}};
constexpr std::array kComp{ControlSpec{choice, "mode", "Mode"},       ControlSpec{knob, "thresholdDb", "Threshold"},
                           ControlSpec{knob, "ratio", "Ratio"},       ControlSpec{knob, "kneeDb", "Knee"},
                           ControlSpec{knob, "attackMs", "Attack"},   ControlSpec{knob, "releaseMs", "Release"},
                           ControlSpec{knob, "blendPct", "Blend"},    ControlSpec{knob, "makeupDb", "Makeup"},
                           ControlSpec{toggle, "autoMakeup", "Auto"}, ControlSpec{knob, "tone", "Tone"}};
constexpr std::array kDrive{ControlSpec{choice, "type", "Type"},    ControlSpec{knob, "drive", "Drive"},
                            ControlSpec{knob, "tone", "Tone"},      ControlSpec{knob, "levelDb", "Level"},
                            ControlSpec{knob, "mixPct", "Mix"},     ControlSpec{knob, "lowCutHz", "Tight"},
                            ControlSpec{toggle, "bright", "Bright"}};
// The amp page edits the active channel. The model of a channel is a preset-only object, not a host parameter
// (parameters.md §2.5), so it is shown by the model note, not by a control.
constexpr std::array kAmp{
    ControlSpec{choice, "channel", "Channel"},        ControlSpec{note, "model", "Model"},
    ControlSpec{knob, "ch{ch}.gain", "Gain"},         ControlSpec{knob, "ch{ch}.bass", "Bass"},
    ControlSpec{knob, "ch{ch}.mid", "Mid"},           ControlSpec{knob, "ch{ch}.treble", "Treble"},
    ControlSpec{knob, "ch{ch}.presence", "Presence"}, ControlSpec{knob, "ch{ch}.depth", "Depth"},
    ControlSpec{knob, "ch{ch}.master", "Master"},     ControlSpec{knob, "ch{ch}.levelDb", "Level"},
    ControlSpec{toggle, "ch{ch}.bright", "Bright"},   ControlSpec{toggle, "ch{ch}.tight", "Tight"},
    ControlSpec{knob, "cleanBlendPct", "Clean blend"}};
constexpr std::array kCab{
    ControlSpec{toggle, "ch{ch}.ir1.enabled", "IR 1"},  ControlSpec{knob, "ch{ch}.ir1.levelDb", "Level"},
    ControlSpec{knob, "ch{ch}.ir1.pan", "Pan"},         ControlSpec{knob, "ch{ch}.ir1.delayMs", "Align"},
    ControlSpec{toggle, "ch{ch}.ir1.invert", "Invert"}, ControlSpec{toggle, "ch{ch}.ir2.enabled", "IR 2"},
    ControlSpec{knob, "ch{ch}.ir2.levelDb", "Level"},   ControlSpec{knob, "ch{ch}.ir2.pan", "Pan"},
    ControlSpec{knob, "ch{ch}.ir2.delayMs", "Align"},   ControlSpec{toggle, "ch{ch}.ir2.invert", "Invert"}};
constexpr std::array kEq{
    ControlSpec{knob, "ch{ch}.g63Db", "63"},           ControlSpec{knob, "ch{ch}.g125Db", "125"},
    ControlSpec{knob, "ch{ch}.g250Db", "250"},         ControlSpec{knob, "ch{ch}.g500Db", "500"},
    ControlSpec{knob, "ch{ch}.g1kDb", "1k"},           ControlSpec{knob, "ch{ch}.g2kDb", "2k"},
    ControlSpec{knob, "ch{ch}.g4kDb", "4k"},           ControlSpec{knob, "ch{ch}.g8kDb", "8k"},
    ControlSpec{knob, "ch{ch}.g16kDb", "16k"},         ControlSpec{knob, "ch{ch}.lowCutHz", "Low cut"},
    ControlSpec{knob, "ch{ch}.highCutHz", "High cut"}, ControlSpec{knob, "ch{ch}.levelDb", "Level"}};
constexpr std::array kMod{ControlSpec{choice, "type", "Type"},   ControlSpec{knob, "rateHz", "Rate"},
                          ControlSpec{choice, "sync", "Sync"},   ControlSpec{knob, "depthPct", "Depth"},
                          ControlSpec{knob, "mixPct", "Mix"},    ControlSpec{knob, "feedbackPct", "Feedback"},
                          ControlSpec{knob, "widthPct", "Width"}};
constexpr std::array kDelay{
    ControlSpec{choice, "mode", "Mode"},        ControlSpec{knob, "timeMs", "Time"},
    ControlSpec{choice, "sync", "Sync"},        ControlSpec{knob, "feedbackPct", "Feedback"},
    ControlSpec{knob, "mixPct", "Mix"},         ControlSpec{knob, "lowCutHz", "Low cut"},
    ControlSpec{knob, "highCutHz", "High cut"}, ControlSpec{choice, "stereo", "Stereo"},
    ControlSpec{knob, "modDepthPct", "Mod"},    ControlSpec{knob, "saturationPct", "Saturation"},
    ControlSpec{knob, "duckDb", "Ducking"}};
constexpr std::array kReverb{ControlSpec{choice, "mode", "Mode"},
                             ControlSpec{knob, "decayS", "Decay"},
                             ControlSpec{knob, "preDelayMs", "Pre-delay"},
                             ControlSpec{knob, "mixPct", "Mix"},
                             ControlSpec{knob, "lowCutHz", "Low cut"},
                             ControlSpec{knob, "highCutHz", "High cut"},
                             ControlSpec{knob, "modPct", "Mod"},
                             ControlSpec{knob, "shimmerPct", "Shimmer"},
                             ControlSpec{choice, "shimmerInterval", "Pitch"},
                             ControlSpec{toggle, "freeze", "Freeze"}};
constexpr std::array kDoubler{ControlSpec{knob, "amountPct", "Amount"}, ControlSpec{knob, "widthPct", "Width"},
                              ControlSpec{knob, "delayMs", "Offset"}, ControlSpec{knob, "detuneCents", "Detune"}};
constexpr std::array kOutput{ControlSpec{knob, "levelDb", "Output"}, ControlSpec{toggle, "limiter", "Limiter"}};
} // namespace

std::span<const ControlSpec> controlsFor(int blockIndex)
{
    switch (blockIndex) {
    case 0:
        return kInput;
    case 1:
        return kGate;
    case 2:
        return kComp;
    case 3:
    case 4:
        return kDrive;
    case kAmpIndex:
        return kAmp;
    case 6:
        return kCab;
    case 7:
        return kEq;
    case 8:
        return kMod;
    case 9:
        return kDelay;
    case 10:
        return kReverb;
    case 11:
        return kDoubler;
    case 12:
        return kOutput;
    default:
        return {};
    }
}

} // namespace osp::ui
