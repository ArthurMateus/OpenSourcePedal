#pragma once

// Dual-grain delay-line pitch shifter for the shimmer feedback path of the Reverb block (issue #73,
// time-based-effects.md §2.2). Two taps read a delay line whose delay sweeps linearly at the slope 1 − ratio (so each
// tap plays back at `ratio` times the input speed) through a window of about W samples, crossfaded with sin² gains
// that are zero where a tap starts and ends its sweep. Sources: the delay-line pitch shifter in the feedback loop of
// Mutable Instruments Clouds' reverb (pichenettes/eurorack, clouds/dsp/fx/pitch_shifter.h, MIT,
// https://github.com/pichenettes/eurorack, accessed 2026-10-27; the structure only, no code); J. O. Smith, PASP,
// "Time-Varying Delay Effects" (https://ccrma.stanford.edu/~jos/pasp/Time_Varying_Delay_Effects.html, accessed
// 2026-10-27): a delay read with slope −ṁ plays at (1 + ṁ)× speed.
//
// Splice alignment: with free-running taps, a tap re-enters at a phase set by f·W, and the crossfaded sum drifts by up
// to half the grain rate: +23 / +15 / −30 cents for octUp / fifthUp / octDown on a 330 Hz sine with 40 ms grains
// (measured, PR description). So when a tap ends its sweep, it restarts half a window from the other tap, moved by the
// lag δ in ±5 ms that maximises the cross-correlation of the two read positions over 5 ms (WSOLA: W. Verhelst and M.
// Roelands, "An overlap-add technique based on waveform similarity (WSOLA) for high quality time-scale modification
// of speech", Proc. IEEE ICASSP 1993, pp. 554-557). The new grain then starts in phase with the sounding one. The
// search is coarse-to-fine (a 4-lag grid, then ±3 lags): about 8 k multiply-adds per splice at 48 kHz (25 splices/s
// per tap for an octave), bounded. The
// output is (g_a·a + g_b·b)/(g_a + g_b): a convex mix, so |out| <= max |in|. FractionalDelay's Catmull-Rom reads.
// Pure C++20; prepare() is the only call that allocates.

#include "osp/dsp/util/FractionalDelay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace osp::dsp {

class GrainShifter {
public:
    // Non-RT. window in seconds (the grain length; 40 ms in the shimmer).
    void prepare(double sampleRate, double windowSeconds)
    {
        window_ = windowSeconds * sampleRate;
        search_ = static_cast<int>(std::lround(0.005 * sampleRate));
        corr_ = search_;
        maxDelay_ = kMinDelay + window_ + search_;
        line_.prepare(static_cast<int>(std::ceil(maxDelay_)) + corr_ + 4);
        reset();
    }

    // RT-safe. Restarts the taps half a window apart.
    void reset() noexcept
    {
        line_.reset();
        const bool down = slope_ < 0.0; // the delay shrinks: pitch up
        taps_[0] = {down ? kMinDelay + window_ : kMinDelay, 0.0, 0.0};
        taps_[1] = {kMinDelay + 0.5 * window_, 0.5, 0.0};
        setRatio(1.0 - slope_);
    }

    // RT-safe. Playback speed: 2 = an octave up, 0.5 = an octave down. Each tap keeps its delay and progress and
    // sweeps the rest of its way at the new slope (no jump).
    void setRatio(double ratio) noexcept
    {
        slope_ = 1.0 - ratio;
        for (Tap& t : taps_)
            t.step = std::abs(slope_) * (1.0 - t.progress) / std::max(remaining(t.delay), 1.0);
    }

    // RT-safe. One sample in, one out. Returns 0 before prepare().
    float process(float x) noexcept
    {
        if (window_ <= 0.0)
            return 0.0f;
        line_.push(x);
        float num = 0.0f, den = 0.0f;
        for (const Tap& t : taps_) {
            const auto s = static_cast<float>(std::sin(std::numbers::pi * t.progress));
            num += s * s * line_.read(static_cast<float>(t.delay));
            den += s * s;
        }
        for (std::size_t i = 0; i < taps_.size(); ++i) {
            Tap& t = taps_[i];
            t.delay += slope_;
            t.progress += t.step;
            if (t.progress >= 1.0)
                splice(t, taps_[1 - i]);
        }
        return den > 1.0e-3f ? num / den : 0.0f;
    }

private:
    static constexpr double kMinDelay = 2.0; // the Catmull-Rom read never needs the not-yet-pushed sample

    struct Tap {
        double delay, progress, step;
    };

    double remaining(double delay) const noexcept
    {
        return slope_ < 0.0 ? delay - kMinDelay : kMinDelay + window_ - delay;
    }

    // Restarts t half a window from `other`, at the lag that best matches other's read position (see the header).
    void splice(Tap& t, const Tap& other) noexcept
    {
        const bool down = slope_ < 0.0;
        const double target = other.delay + (down ? 0.5 : -0.5) * window_;
        int lo = static_cast<int>(std::lround(target)) - search_, hi = lo + 2 * search_;
        lo = std::max(lo, static_cast<int>(kMinDelay) + 1);
        hi = std::min(hi, static_cast<int>(maxDelay_));
        const int ref = static_cast<int>(std::lround(other.delay));
        int best = std::clamp(static_cast<int>(std::lround(target)), lo, std::max(lo, hi));
        // Coarse pass on a 4-lag grid (every 4th sample), then the best ±3 lags (every 2nd sample).
        auto search = [&](int from, int to, int lagStep, int sampleStep) {
            float bestCorr = -1.0e30f;
            for (int d = std::max(from, lo); d <= std::min(to, hi); d += lagStep) {
                float c = 0.0f;
                for (int k = 0; k < corr_; k += sampleStep)
                    c += line_.read(static_cast<float>(d + k)) * line_.read(static_cast<float>(ref + k));
                if (c > bestCorr) {
                    bestCorr = c;
                    best = d;
                }
            }
        };
        search(lo, hi, 4, 4);
        search(best - 3, best + 3, 1, 2);
        t.delay = best + (other.delay - ref); // keep the other tap's fractional part
        t.progress = 0.0;
        t.step = slope_ == 0.0 ? 0.0 : std::abs(slope_) / std::max(remaining(t.delay), 1.0);
    }

    FractionalDelay line_;
    std::array<Tap, 2> taps_{};
    double window_ = 0.0, maxDelay_ = 0.0, slope_ = 0.0;
    int search_ = 0, corr_ = 0;
};

} // namespace osp::dsp
