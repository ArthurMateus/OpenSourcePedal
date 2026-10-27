// Delay voice, analog (BBD) and tape voicings (#71 part 3): echo time with the loop oversampler and ADAA, no runaway
// at 100 % feedback and saturation, wow/flutter and clock depth, saturation THD and aliasing at 2×, dry paths, mix
// ramp residual, rate × block matrix under the RT checker, reset and a 1000-vector NaN fuzz.

#include "osp/dsp/delay/DelayVoice.h"

#include "measure.hpp"
#include "rt_checker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using osp::dsp::DelayVoice;
using Stereo = osp::dsp::DelayVoice::Stereo;
using Voicing = osp::dsp::DelayVoice::Voicing;

namespace {

constexpr int kMaxBlock = 2048;

std::vector<float> sine(double hz, double fs, int n, double amp)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

void render(DelayVoice& v, std::vector<float>& l, std::vector<float>& r, int block, bool wet)
{
    const int total = static_cast<int>(l.size());
    const int step = block > 0 ? block : 64;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int pos = 0; pos < total; pos += step) {
            const int n = std::min(step, total - pos);
            float *pl = l.data() + pos, *pr = r.data() + pos;
            if (block == 0)
                v.process(pl, pr, 0);
            if (wet)
                v.processWet(pl, pr, pl, pr, n);
            else
                v.process(pl, pr, n);
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    OSP_REQUIRE_OUTPUT_CLEAN(l);
    OSP_REQUIRE_OUTPUT_CLEAN(r);
}

void setup(DelayVoice& v, Voicing voicing, double fs, float ms, float fb, float sat, float mod, Stereo s)
{
    v.setVoicing(voicing);
    v.setStereo(s);
    v.setTimeMs(ms);
    v.setFeedbackPct(fb);
    v.setSaturationPct(sat);
    v.setModDepthPct(mod);
    v.setLowCutHz(50.0f);
    v.setHighCutHz(16000.0f);
    v.prepare(fs, kMaxBlock);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t len)
{
    double s = 0.0;
    for (std::size_t i = from; i < from + len; ++i)
        s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(len));
}

// Largest relative deviation of the per-cycle frequency (from linearly interpolated rising zero crossings) from f0.
double maxFreqDeviation(const std::vector<float>& y, std::size_t from, double fs, double f0)
{
    double last = -1.0, worst = 0.0;
    for (std::size_t i = from + 1; i < y.size(); ++i) {
        if (y[i - 1] < 0.0f && y[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + y[i - 1] / static_cast<double>(y[i - 1] - y[i]);
            if (last >= 0.0)
                worst = std::max(worst, std::abs(fs / (t - last) / f0 - 1.0));
            last = t;
        }
    }
    return worst;
}

} // namespace

TEST_CASE("DelayVoice voicings: echo time within +-1 sample through the 2x loop oversampler", "[dsp][delay]")
{
    const double fs = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const float ms = GENERATE(7.0f, 400.0f, 3000.0f);
    const auto voicing = GENERATE(Voicing::analog, Voicing::tape);
    CAPTURE(fs, ms, static_cast<int>(voicing));
    DelayVoice v;
    setup(v, voicing, fs, ms, 0.0f, 0.0f, 0.0f, Stereo::mono);
    CHECK(v.getLoopOversampling() == (fs < 88200.0 ? 2 : 1));
    const double d = ms * fs / 1000.0;
    const auto len = static_cast<std::size_t>(d + 0.2 * fs);
    std::vector<float> l(len, 0.0f), r(len, 0.0f);
    l[0] = r[0] = 0.1f;
    render(v, l, r, 64, true);
    // The echo's 1 kHz group delay (ADR-0005 D4's measure); the 50 Hz low cut adds at most 0.76 sample there.
    const double w = 2.0 * std::numbers::pi * 1000.0 / fs;
    std::complex<double> num, den;
    for (std::size_t i = 0; i < len; ++i) {
        const auto e = std::polar(static_cast<double>(l[i]), -w * static_cast<double>(i));
        num += static_cast<double>(i) * e;
        den += e;
    }
    CAPTURE((num / den).real() - d);
    WARN("echo time error " << (num / den).real() - d);
    CHECK(std::abs((num / den).real() - d) <= 1.0);
}

TEST_CASE("DelayVoice voicings: no runaway at 100 % feedback, saturation and modulation", "[dsp][delay]")
{
    const double fs = 48000.0;
    const auto voicing = GENERATE(Voicing::analog, Voicing::tape);
    const auto mode = GENERATE(Stereo::mono, Stereo::pingpong, Stereo::wide);
    CAPTURE(static_cast<int>(voicing), static_cast<int>(mode));
    DelayVoice v;
    setup(v, voicing, fs, 150.0f, 100.0f, 100.0f, 100.0f, mode);
    const auto sec = static_cast<std::size_t>(fs);
    std::vector<float> l(12 * sec, 0.0f), r(12 * sec, 0.0f);
    std::mt19937 rng(71);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (std::size_t i = 0; i < sec; ++i)
        l[i] = r[i] = noise(rng); // 1 s of full-scale noise, then silence
    render(v, l, r, 127, true);
    float peak = 0.0f;
    for (std::size_t i = 0; i < l.size(); ++i)
        peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
    const double early = rms(l, sec, sec), late = rms(l, 11 * sec, sec);
    WARN("peak " << peak << ", rms 1-2 s " << 20.0 * std::log10(early) << " dB, 11-12 s " << 20.0 * std::log10(late)
                 << " dB");
    CHECK(peak < 4.0f);
    CHECK(late < 0.5 * early);
}

TEST_CASE("DelayVoice voicings: wow/flutter and BBD clock depth, saturation THD and 2x aliasing", "[dsp][delay]")
{
    const double fs = 48000.0;
    SECTION("modulation depth 100 % at 400 ms: tape +-(0.80 + 0.12) %, BBD clock +-0.42 %")
    {
        for (auto voicing : {Voicing::tape, Voicing::analog}) {
            DelayVoice v;
            setup(v, voicing, fs, 400.0f, 0.0f, 0.0f, 100.0f, Stereo::mono);
            auto l = sine(1000.0, fs, static_cast<int>(3.0 * fs), 0.1), r = l;
            render(v, l, r, 64, true);
            const double dev = maxFreqDeviation(l, static_cast<std::size_t>(0.5 * fs), fs, 1000.0);
            WARN((voicing == Voicing::tape ? "tape" : "bbd") << " max frequency deviation " << 100.0 * dev << " %");
            if (voicing == Voicing::tape)
                CHECK((dev > 0.0080 && dev < 0.0100));
            else
                CHECK((dev > 0.0038 && dev < 0.0046));
        }
        DelayVoice still;
        setup(still, Voicing::tape, fs, 400.0f, 0.0f, 0.0f, 0.0f, Stereo::mono);
        auto l = sine(1000.0, fs, static_cast<int>(1.5 * fs), 0.1), r = l;
        render(still, l, r, 64, true);
        CHECK(maxFreqDeviation(l, static_cast<std::size_t>(0.5 * fs), fs, 1000.0) < 1e-4);
    }
    SECTION("saturation: THD rises with the knob; aliases stay low at 2x + ADAA1")
    {
        const auto tone = osp::measure::coherentTone(2000.0, fs);
        for (auto voicing : {Voicing::tape, Voicing::analog}) {
            double thd[2] = {};
            for (int s = 0; s < 2; ++s) {
                DelayVoice v;
                setup(v, voicing, fs, 7.0f, 0.0f, s == 0 ? 0.0f : 100.0f, 0.0f, Stereo::mono);
                auto l = osp::measure::makeTone(tone, fs, -3.0,
                                                osp::measure::kCoherentFftSize + osp::measure::kWarmupSamples);
                auto r = l;
                render(v, l, r, 64, true);
                const auto rep = osp::measure::analyseCoherent(l, tone.bin);
                thd[s] = rep.thd;
                WARN((voicing == Voicing::tape ? "tape" : "bbd")
                     << " sat " << (s == 0 ? 0 : 100) << " %: THD " << 100.0 * rep.thd << " %, worst alias "
                     << rep.worstAliasDbc << " dBc");
                if (s == 1)
                    CHECK(rep.worstAliasDbc <= -70.0);
            }
            CHECK(thd[0] < 0.005); // the BBD's bias leaves 0.33 % of 2nd harmonic at -3 dBFS even at 0 %
            CHECK(thd[1] > 0.01);
        }
    }
}

TEST_CASE("DelayVoice voicings: mix 0, disabled and bypass are the exact dry; mix ramp residual", "[dsp][delay]")
{
    const double fs = 48000.0;
    const auto a = sine(220.0, fs, static_cast<int>(0.4 * fs), 0.4),
               b = sine(330.0, fs, static_cast<int>(0.4 * fs), 0.3);
    for (auto voicing : {Voicing::analog, Voicing::tape}) {
        CAPTURE(static_cast<int>(voicing));
        for (int mode = 0; mode < 3; ++mode) {
            DelayVoice v;
            setup(v, voicing, fs, 20.0f, 100.0f, 100.0f, 100.0f, Stereo::pingpong);
            if (mode == 0)
                v.setMixPct(0.0f);
            else if (mode == 1)
                v.setEnabled(false);
            else
                v.setBypassed(true);
            v.reset();
            auto l = a, r = b;
            render(v, l, r, 127, false);
            CHECK(l == a);
            CHECK(r == b);
        }
        // Mix 25 -> 100 % against dry + the 20 ms ramp · wet (REQ-TEST-003 <= -60 dBc).
        DelayVoice w, v;
        setup(w, voicing, fs, 50.0f, 40.0f, 60.0f, 50.0f, Stereo::wide);
        setup(v, voicing, fs, 50.0f, 40.0f, 60.0f, 50.0f, Stereo::wide);
        auto wl = a, wr = b, l = a, r = b;
        render(w, wl, wr, 64, true);
        v.setMixPct(100.0f);
        render(v, l, r, 64, false);
        osp::dsp::LinearRamp law{0.25f};
        law.prepare(fs, 0.020);
        law.setTarget(1.0f);
        std::vector<double> ref(a.size());
        for (std::size_t i = 0; i < a.size(); ++i)
            ref[i] = a[i] + law.next() * static_cast<double>(wl[i]);
        const double res = osp::measure::residualDbc(l, ref);
        WARN("mix ramp residual " << res << " dBc");
        CHECK(res <= -60.0);
    }
}

TEST_CASE("DelayVoice voicings: every rate x block size is bit-identical under the RT checker; reset()", "[dsp][delay]")
{
    const double rate = GENERATE(44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE(0, 1, 127, 2048);
    const auto voicing = GENERATE(Voicing::analog, Voicing::tape);
    CAPTURE(rate, block, static_cast<int>(voicing));
    const auto in = sine(440.0, rate, static_cast<int>(0.3 * rate), 0.5);
    DelayVoice ref, v;
    for (DelayVoice* p : {&ref, &v}) {
        setup(*p, voicing, rate, 30.0f, 70.0f, 50.0f, 80.0f, Stereo::pingpong);
        p->setTimeMs(45.0f); // a glide (moving the BBD filter), a saturation and a depth ramp inside the render
        p->setSaturationPct(90.0f);
        p->setModDepthPct(20.0f);
    }
    auto rl = in, rr = in, l = in, r = in;
    render(ref, rl, rr, 64, false);
    render(v, l, r, block, false);
    CHECK(l == rl);
    CHECK(r == rr);
    v.reset();
    ref.reset();
    auto l2 = in, r2 = in, rl2 = in, rr2 = in;
    render(v, l2, r2, 127, false);
    render(ref, rl2, rr2, 64, false);
    CHECK(l2 == rl2); // reset() is bit-identical
}

TEST_CASE("DelayVoice voicings: 1000 random parameter vectors incl. NaN/Inf under the RT checker", "[dsp][delay]")
{
    DelayVoice v;
    setup(v, Voicing::tape, 44100.0, 25.0f, 90.0f, 50.0f, 50.0f, Stereo::wide);
    std::mt19937 rng(713);
    std::uniform_real_distribution<float> pct(-20.0f, 120.0f), ms(-10.0f, 3500.0f), hz(0.0f, 20000.0f),
        sig(-1.0f, 1.0f);
    std::uniform_int_distribution<int> len(0, kMaxBlock), coin(0, 1), three(0, 2);
    std::vector<float> bl(kMaxBlock), br(kMaxBlock);
    int nonFinite = 0;
    const auto rt = osp::rt::test::runAsAudioThread([&] {
        for (int k = 0; k < 1000; ++k) {
            v.setVoicing(static_cast<Voicing>(three(rng)));
            v.setStereo(static_cast<Stereo>(three(rng)));
            v.setTimeMs(k % 71 == 0 ? std::nanf("") : ms(rng));
            v.setFeedbackPct(k % 61 == 0 ? std::nanf("") : pct(rng));
            v.setMixPct(pct(rng));
            v.setSaturationPct(k % 59 == 0 ? INFINITY : pct(rng));
            v.setModDepthPct(k % 53 == 0 ? std::nanf("") : pct(rng));
            v.setLowCutHz(hz(rng));
            v.setHighCutHz(k % 67 == 0 ? std::nanf("") : hz(rng));
            v.setEnabled(coin(rng) == 1);
            const int n = len(rng);
            for (int i = 0; i < n; ++i) {
                bl[static_cast<std::size_t>(i)] = k % 97 == 0 && i == 0 ? std::nanf("") : 4.0f * sig(rng);
                br[static_cast<std::size_t>(i)] = k % 89 == 0 && i == 0 ? INFINITY : sig(rng);
            }
            v.process(bl.data(), br.data(), n);
            for (int i = 0; i < n; ++i)
                nonFinite +=
                    std::isfinite(bl[static_cast<std::size_t>(i)]) && std::isfinite(br[static_cast<std::size_t>(i)])
                        ? 0
                        : 1;
        }
    });
    OSP_REQUIRE_RT_CLEAN(rt);
    CHECK(nonFinite == 0);
}
