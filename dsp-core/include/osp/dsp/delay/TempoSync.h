#pragma once

// `delay.sync` (parameters.md §3.4): note divisions from 1/64 triplet to 1/1 dotted, converted to a delay time at the
// host tempo. Pure C++20, constexpr where it can be.
//
// A whole note (1/1) is 4 beats; dotted is 1.5×, triplet 2/3× the straight value. The result is clamped to the
// `delay.timeMs` range, 7…3000 ms, so a slow tempo with a long division never exceeds the delay line.

namespace osp::dsp {

// Enum order as in parameters.md: `off`, then per note value triplet, straight, dotted, from 1/64 to 1/1.
// clang-format off
enum class SyncDivision : int {
    off,
    n1_64t, n1_64, n1_64d,
    n1_32t, n1_32, n1_32d,
    n1_16t, n1_16, n1_16d,
    n1_8t, n1_8, n1_8d,
    n1_4t, n1_4, n1_4d,
    n1_2t, n1_2, n1_2d,
    n1_1t, n1_1, n1_1d,
};
// clang-format on

struct TempoSync {
    static constexpr double kMinMs = 7.0;
    static constexpr double kMaxMs = 3000.0;
    static constexpr int kNumDivisions = 22; // including off

    // Length in beats (quarter notes), 0 for off.
    static constexpr double beats(SyncDivision d) noexcept
    {
        const int i = static_cast<int>(d);
        if (i <= 0 || i >= kNumDivisions)
            return 0.0;
        const int note = (i - 1) / 3;  // 0 = 1/64 … 6 = 1/1
        const int variant = (i - 1) % 3; // 0 triplet, 1 straight, 2 dotted
        double b = 4.0 / static_cast<double>(64 >> note);
        if (variant == 0)
            b *= 2.0 / 3.0;
        else if (variant == 2)
            b *= 1.5;
        return b;
    }

    // Delay time in ms at `bpm`, clamped to 7…3000 ms. Returns a negative value for off or a non-positive/NaN tempo,
    // so the caller keeps using `delay.timeMs`.
    static double timeMs(SyncDivision d, double bpm) noexcept
    {
        const double b = beats(d);
        if (b <= 0.0 || !(bpm > 0.0))
            return -1.0;
        const double ms = 60000.0 / bpm * b;
        return ms < kMinMs ? kMinMs : (ms > kMaxMs ? kMaxMs : ms);
    }
};

} // namespace osp::dsp
