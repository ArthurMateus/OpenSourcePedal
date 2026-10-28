// osp-bench entries: drive. Self-registering (see bench_common.h), so adding a block touches no shared file.

#include "bench_common.h"

#include "osp/dsp/Drive.h"
#include "osp/dsp/DriveRegion.h"
#include "osp/dsp/oversampling/Oversampler.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace osp::bench {

    // One drive slot (mid, defaults) inside a 4x IIR region: OS-1 at 48 kHz with one drive enabled (ADR-0005 D2).
OSP_BENCH(drive_stage_4x)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::Oversampler osDrive;
    osDrive.prepare(kSampleRate, kBlock, 4);
    osp::dsp::DriveStage drive;
    drive.prepare(kSampleRate * 4, kBlock * 4);
    results.push_back(
        {"drive_stage_4x", measure(bench, "drive_stage_4x", input, [&](const float* in, float* out, int n) {
             osDrive.process(in, out, n, [&](float* x, int osN) { drive.process(x, osN); });
         })});
}

    // OS-1 with both slots enabled (mid into dist), Live at 48 kHz: the full drive section of a stacked preset.
OSP_BENCH(drive_region_live)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::DriveRegion os1;
    os1.slot(0).setEnabled(true);
    os1.slot(1).setEnabled(true);
    os1.slot(1).setType(osp::dsp::DriveType::dist);
    os1.prepare(kSampleRate, kBlock);
    results.push_back(
        {"drive_region_live", measure(bench, "drive_region_live", input,
                                      [&](const float* in, float* out, int n) { os1.process(in, out, n); })});
}

// The same stacked drive section in HQ at 48 kHz (#112): 8x on the FIR kind (ADR-0005 A3) plus the 6-sample pad.
OSP_BENCH(drive_region_hq)
{
    auto& bench = ctx.bench;
    const auto& input = ctx.input;
    auto& results = ctx.results;
    osp::dsp::DriveRegion os1Hq;
    os1Hq.slot(0).setEnabled(true);
    os1Hq.slot(1).setEnabled(true);
    os1Hq.slot(1).setType(osp::dsp::DriveType::dist);
    os1Hq.prepare(kSampleRate, kBlock, osp::dsp::OversamplingQuality::hq);
    results.push_back(
        {"drive_region_hq", measure(bench, "drive_region_hq", input,
                                    [&](const float* in, float* out, int n) { os1Hq.process(in, out, n); })});
}

} // namespace osp::bench
