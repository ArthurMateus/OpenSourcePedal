// Parameter fan-out (ADR-0009 D8): every block's parameters, read once per block and passed to its setters.
// The setters smooth per sample (parameters.md §1.4); setting an unchanged value is a no-op for every block.

#include "GraphIds.h"

#include <cmath>

namespace osp::engine {

void Graph::applyParameters() noexcept
{
    const Ids& id = *ids_;

    inputOn_.setTarget(on(id.input[0]));
    input_.setGainDb(value(id.input[1]));

    const auto& g = id.gate;
    dsp::Gate::Params gp;
    gp.thresholdDb = value(g[kGateThreshold]);
    gp.hysteresisDb = value(g[kGateHysteresis]);
    gp.attackMs = value(g[kGateAttack]);
    gp.holdMs = value(g[kGateHold]);
    gp.releaseMs = value(g[kGateRelease]);
    gp.rangeDb = value(g[kGateRange]);
    gp.detector = choice(g[kGateDetector]) == 1 ? dsp::Gate::Detector::Rms : dsp::Gate::Detector::Peak;
    gate_.setParameters(gp);
    gate_.setEnabled(on(g[kGateEnabled]));

    const auto& c = id.comp;
    compOn_.setTarget(on(c[kCompEnabled]));
    comp_.setThresholdDb(value(c[kCompThreshold]));
    comp_.setRatio(value(c[kCompRatio]));
    comp_.setKneeDb(value(c[kCompKnee]));
    comp_.setAttackMs(value(c[kCompAttack]));
    comp_.setReleaseMs(value(c[kCompRelease]));
    comp_.setBlendPct(value(c[kCompBlend]));
    comp_.setMakeupDb(value(c[kCompMakeup]));
    comp_.setAutoMakeup(on(c[kCompAutoMakeup]));
    comp_.setMode(static_cast<dsp::CompMode>(choice(c[kCompMode])));
    comp_.setTone(value(c[kCompTone]));

    for (int k = 0; k < dsp::DriveRegion::kNumSlots; ++k) {
        const auto& d = id.drive[static_cast<std::size_t>(k)];
        auto& slot = drives_.slot(k);
        slot.setType(static_cast<dsp::DriveType>(choice(d[kDriveType])));
        slot.setDrive(value(d[kDriveDrive]));
        slot.setTone(value(d[kDriveTone]));
        slot.setLevelDb(value(d[kDriveLevel]));
        slot.setLowCutHz(value(d[kDriveLowCut]));
        slot.setBright(on(d[kDriveBright]));
        slot.setMixPct(value(d[kDriveMix]));
        slot.setEnabled(on(d[kDriveEnabled]));
    }

    for (int s = 0; s < kNumChannels; ++s)
        applyStrip(s);

    output_.setLevelDb(value(id.output[0]));
    output_.setLimiter(on(id.output[1]));
}

void Graph::applyStrip(int strip) noexcept
{
    const Ids& id = *ids_;
    auto& s = strips_[static_cast<std::size_t>(strip)];

    const auto& a = id.amp[static_cast<std::size_t>(strip)];
    AmpParams ap;
    ap.gain = value(a[kAmpGain]);
    ap.bass = value(a[kAmpBass]);
    ap.mid = value(a[kAmpMid]);
    ap.treble = value(a[kAmpTreble]);
    ap.presence = value(a[kAmpPresence]);
    ap.depth = value(a[kAmpDepth]);
    ap.master = value(a[kAmpMaster]);
    ap.levelDb = value(a[kAmpLevel]);
    ap.bright = on(a[kAmpBright]);
    ap.tight = on(a[kAmpTight]);
    s.amp->setParameters(ap);

    // amp.cabLink / eq.followChannel off: ch1's values for every strip (parameters.md §3.1, §3.2).
    const auto& c = id.cab[static_cast<std::size_t>(on(id.ampCabLink) ? strip : 0)];
    s.cab.setEnabled(on(id.cabEnabled));
    for (int k = 0; k < dsp::Cab::kNumSlots; ++k) {
        const int o = k * kCabKeysPerIr;
        s.cab.setSlotEnabled(k, on(c[static_cast<std::size_t>(o + kCabIrEnabled)]));
        s.cab.setLevelDb(k, value(c[static_cast<std::size_t>(o + kCabIrLevel)]));
        s.cab.setPanPct(k, value(c[static_cast<std::size_t>(o + kCabIrPan)]));
        s.cab.setInvert(k, on(c[static_cast<std::size_t>(o + kCabIrInvert)]));
        s.cab.setDelayMs(k, value(c[static_cast<std::size_t>(o + kCabIrDelay)]));
    }

    const auto& e = id.eq[static_cast<std::size_t>(on(id.eqFollow) ? strip : 0)];
    const auto at = [&](int k) { return value(e[static_cast<std::size_t>(k)]); };
    s.eqOnL.setTarget(on(id.eqEnabled));
    s.eqOnR.setTarget(on(id.eqEnabled));
    s.eq.setMode(static_cast<dsp::EqMode>(choice(e[kEqMode])));
    for (int b = 0; b < 9; ++b)
        s.eq.setGraphic9Db(b, at(kEqGraphic9 + b));
    for (int b = 0; b < 5; ++b)
        s.eq.setGraphic5Db(b, at(kEqGraphic5 + b));
    for (int b = 0; b < 4; ++b)
        s.eq.setParametric(b, at(kEqParametric + 3 * b), at(kEqParametric + 3 * b + 1), at(kEqParametric + 3 * b + 2));
    s.eq.setEdgeShape(0, static_cast<dsp::EqEdgeShape>(choice(e[kEqP1Shape])));
    s.eq.setEdgeShape(3, static_cast<dsp::EqEdgeShape>(choice(e[kEqP4Shape])));
    s.eq.setLowCutHz(at(kEqLowCut));
    s.eq.setHighCutHz(at(kEqHighCut));
    s.eq.setLevelDb(at(kEqLevel));
}

} // namespace osp::engine
