#include "osp/dsp/tuner/PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace osp::dsp {

namespace {
constexpr double kWeakThreshold = 0.35; // no dip below kThreshold: accept the global minimum up to this, else unvoiced
} // namespace

void PitchDetector::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    minLag_ = std::max(2, static_cast<int>(std::floor(sampleRate / kMaxHz)));
    maxLag_ = static_cast<int>(std::ceil(sampleRate / kMinHz));
    window_ = maxLag_; // one period of the lowest note (see the header)
    diff_.assign(static_cast<std::size_t>(maxLag_) + 2, 0.0);
    cmnd_.assign(static_cast<std::size_t>(maxLag_) + 2, 1.0);
}

void PitchDetector::setReferenceHz(double hz) noexcept
{
    referenceHz_ = std::isnan(hz) ? 440.0 : std::clamp(hz, kMinReferenceHz, kMaxReferenceHz);
}

TunerReading PitchDetector::toNote(double hz, double referenceHz) noexcept
{
    TunerReading r;
    if (!(hz > 0.0) || !std::isfinite(hz))
        return r;
    const double semis = 12.0 * std::log2(hz / referenceHz);
    r.valid = true;
    r.hz = hz;
    r.midiNote = 69 + static_cast<int>(std::lround(semis));
    r.cents = 100.0 * (semis - static_cast<double>(r.midiNote - 69));
    return r;
}

TunerReading PitchDetector::detect(const float* x, int n) noexcept
{
    if (sampleRate_ <= 0.0 || x == nullptr || n < requiredSamples())
        return {};
    const float* s = x + (n - requiredSamples());

    // Level gate on the integration window (non-finite input reads as silence).
    double energy = 0.0;
    for (int j = 0; j < window_; ++j)
        energy += static_cast<double>(s[j]) * s[j];
    if (!std::isfinite(energy) || 10.0 * std::log10(energy / window_ + 1e-300) < kGateDbfs)
        return {};

    // YIN steps 2-3: difference function and its cumulative mean normalisation.
    double running = 0.0;
    for (int tau = 1; tau <= maxLag_ + 1; ++tau) {
        double d = 0.0;
        for (int j = 0; j < window_; ++j) {
            const double e = static_cast<double>(s[j]) - s[j + tau];
            d += e * e;
        }
        diff_[static_cast<std::size_t>(tau)] = d;
        running += d;
        cmnd_[static_cast<std::size_t>(tau)] = running > 0.0 ? d * tau / running : 1.0;
    }

    // Step 4: the first dip below the threshold, followed down to its local minimum (avoids octave-up errors).
    int tau = -1;
    for (int t = minLag_; t <= maxLag_; ++t)
        if (cmnd_[static_cast<std::size_t>(t)] < kThreshold) {
            while (t + 1 <= maxLag_ && cmnd_[static_cast<std::size_t>(t) + 1] < cmnd_[static_cast<std::size_t>(t)])
                ++t;
            tau = t;
            break;
        }
    if (tau < 0) {
        tau = minLag_;
        for (int t = minLag_ + 1; t <= maxLag_; ++t)
            if (cmnd_[static_cast<std::size_t>(t)] < cmnd_[static_cast<std::size_t>(tau)])
                tau = t;
        if (cmnd_[static_cast<std::size_t>(tau)] >= kWeakThreshold)
            return {};
    }

    // Step 5: parabolic interpolation of d(tau) around the minimum.
    double frac = 0.0;
    if (tau > 1 && tau <= maxLag_) {
        const double a = diff_[static_cast<std::size_t>(tau) - 1], b = diff_[static_cast<std::size_t>(tau)],
                     c = diff_[static_cast<std::size_t>(tau) + 1];
        const double den = a - 2.0 * b + c;
        if (den > 0.0)
            frac = std::clamp(0.5 * (a - c) / den, -1.0, 1.0);
    }
    TunerReading r = toNote(sampleRate_ / (tau + frac), referenceHz_);
    r.confidence = std::clamp(1.0 - cmnd_[static_cast<std::size_t>(tau)], 0.0, 1.0);
    return r;
}

} // namespace osp::dsp
