# Oversampling and anti-aliasing for nonlinear blocks

- Issue: #10
- Author: acct1/researcher (Sable)
- Date: 2026-10-25 (all sources accessed on that date unless stated otherwise)
- Status: research note. The architect turns it into an ADR and `docs/spec/` RT/DSP rules.

## 1. Summary

1. **Combine ADAA with modest oversampling. Do not rely on oversampling alone.** First-order ADAA "markedly reduces
   aliasing distortion, especially in combination with low order oversampling" [S1]. Higher-order ADAA improves the
   signal-to-noise ratio further and "require[s] fewer operations than oversampling" [S2].
2. **Default factors by block** (proposal; each is checked by the alias test in §3.3, *unverified* until measured):
   2× + ADAA1 for soft-clip drives, 4× + ADAA1 for high-gain amp stages, 4× + ADAA2 for hard clip and fuzz.
   At 88.2/96 kHz host rates, halve the factor. An offline/"HQ" render mode doubles it.
3. **Filters: polyphase IIR halfband by default (live), FIR linear-phase halfband as an option (HQ/offline).** JUCE's own
   documentation states the trade-off: FIR gives linear phase with maximum latency; IIR gives minimal latency, with the
   phase "compromised around the Nyquist frequency" [S5].
4. **`dsp-core` cannot use `juce::dsp::Oversampling`**, because ADR-0001 keeps JUCE out of `dsp-core` [S12]. Use **HIIR**
   (Laurent de Soras, two-path polyphase IIR halfband with SSE/AVX/NEON paths, v1.50; **WTFPL-2.0** per the archive's `license.txt` and headers) for IIR
   [S6, S7], plus our own small FIR halfband for the linear-phase mode. JUCE's design parameters are a good default
   to copy (§2.2).
5. **ADAA adds delay.** ADAA1 has a group delay of 0.5 sample and ADAA2 of 1 sample. Both also roll off highs:
   ADAA1 has a zero at Nyquist, and ADAA2 is about −9.5 dB at Nyquist (derived from [S1] eq. 17–18). Compensate
   inside the block, or merge the delay into an adjacent filter as [S1] §6 does, and count it in the reported latency.
   Under N× oversampling, the delay is 0.5/N or 1/N base-rate samples, and the roll-off moves up to the oversampled
   Nyquist, where the decimator removes most of it.
6. **NAM models are not oversampled internally.** The reference plugin only resamples to the model rate, and users
   report aliasing folding at 24 kHz [S9, S10]. Running an A1 WaveNet at an integer multiple of its training rate
   (dilation scaling) reduces aliasing "with a significant performance cost" [S11]. Recommendation: native rate by
   default, with a 2× "HQ" option where the runtime supports it. See the `nam-runtime.md` note (#6).
7. **Measure aliasing with a sine-sweep alias-to-signal ratio (ASR)** test per nonlinear block, at 44.1 and 48 kHz,
   with fixed per-block thresholds (§3.3). This follows the ASR metric of [S8] and the perceptual framing of [S3].
8. **Report total latency** = oversampler round-trip + ADAA delay + any linear-phase filter delay, rounded up to an
   integer, through `setLatencySamples`. Pad with an integer delay so the reported value is exact (JUCE offers
   `useIntegerLatency` for this [S5]).

## 2. Findings

### 2.1 Techniques

| Technique | What it does | Cost | Delay | Source |
|---|---|---|---|---|
| Oversampling N× | Raises Nyquist so that fewer harmonics fold back. Needs an up-filter and a down-filter. | ≈ N× the nonlinear block + filters | Filter dependent (IIR small, FIR large) | S1, S5 |
| ADAA 1st order | `y[n] = (F1(x[n]) − F1(x[n−1])) / (x[n] − x[n−1])`, with `F1` the antiderivative of `f` | 1 antiderivative + 1 division per sample | 0.5 sample; linear-case response `(x[n]+x[n−1])/2` | S1, S4 |
| ADAA 2nd order | Uses `F2`, three taps, more divisions and fallbacks | ≈ 2–3× ADAA1 (*unverified*) | 1 sample; linear case `(x[n]+4x[n−1]+x[n−2])/6` in [S1] eq. 18, `(x[n]+x[n−1]+x[n−2])/3` for [S2]'s form (same delay, −9.54 dB at Nyquist; per review derivation) | S1, S2, S4 |
| Ill-conditioning fallback | If `|x[n] − x[n−1]| < TOL`, use `f((x[n] + x[n−1]) / 2)` | branch | n/a | S1 §2.3, S4 |
| Stateful ADAA | ADAA inside state-space or one-port-nonlinear circuits (diode clipper in a filter) | higher | as above | S13 |
| (poly)BLAMP | Post-corrects samples near clipping corners with a band-limited ramp. Hard and soft clippers only. | low | few samples | S14, S15 |
| Smoother NN activations | Lowers ASR in neural amp models without a substantial ESR increase (requires retraining) | none at runtime | 0 | S8 |

Notes:
- ADAA needs a closed-form antiderivative (`F1`, and `F2` for second order). This is simple for tanh, hard clip, and
  polynomial or piecewise shapers. For tanh, `F1 = log(cosh x)`, which needs an overflow-safe form for large |x|
  (*unverified*: standard identity, implementation detail).
- Parker et al. report that a lowpass inserted inside feedback loops is "not strongly audible at sampling rates of
  88.2 kHz or above" [S1]. That supports using ADAA together with 2× oversampling inside feedback structures.

### 2.2 Halfband filter options

| Option | Licence | AGPLv3-compatible | Phase | Notes | Source |
|---|---|---|---|---|---|
| `juce::dsp::Oversampling` (JUCE 9.0.4) | AGPLv3 / commercial | Yes, but `app/` & `ui/` only (ADR-0001) | IIR polyphase or FIR equiripple halfband | 2/4/8/16× as cascaded 2× stages; `getLatencyInSamples()`, `useIntegerLatency` | S5 |
| HIIR v1.50 (de Soras) | **WTFPL-2.0** (SPDX `WTFPL`), per the archive's `license.txt` and every header. The site footer's "CC0 or WTFPL" does not apply, because the archive states its own licence. | Yes: the FSF lists WTFPL v2 as "a lax permissive non-copyleft free software license, compatible with the GNU GPL" | Two-path polyphase IIR (allpass) | SSE/SSE2/AVX/AVX-512/NEON. `unevens/hiir` is a third-party mirror (last push 2021-11-01). Vendor upstream 1.50 instead. | S6, S7, S16 |
| r8brain-free-src | MIT | Yes | Linear/minimum-phase SRC | General-purpose SRC, heavier than needed for 2× stages | S17 |
| chowdsp_utils | Mixed: BSD (core) / **GPLv3** (most DSP modules) | Yes (GPLv3 → AGPLv3 OK) | n/a | JUCE module; check each module's licence | S18 |

JUCE design defaults per 2× stage n (0 = first stage, at the highest rate) [S5, `juce_Oversampling.cpp`]:

| Parameter | Max quality | Normal |
|---|---|---|
| Transition width up (normalised) | 0.10 × (0.5 if n = 0) | 0.12 × (0.5 if n = 0) |
| Transition width down | 0.12 × (0.5 if n = 0) | 0.15 × (0.5 if n = 0) |
| Stopband up | −90 dB + 10 dB·n | −70 dB + 8 dB·n |
| Stopband down | −75 dB + 10 dB·n | −60 dB + 8 dB·n |

Kahles et al. evaluated decimators for 8× oversampled hard clipping at 44.1 kHz, using the A-weighted noise-to-mask
ratio on tones from 27.5 Hz to 4.186 kHz. Linear interpolation with a high-shelf EQ was a sufficient upsampler. Among
the decimators tested, only the cascaded integrator-comb (CIC) gave perceptually sufficient suppression across the
whole range [S3]. Their filters met only ±1 dB of ripple and 40 dB of stopband attenuation, so a cheap upsampler is
acceptable but the decimator matters most [S3].

### 2.3 Neural amp models (NAM) and aliasing

| Finding | Source |
|---|---|
| The NAM plugin resamples host↔model rate (Lanczos). It does not oversample beyond the model rate. | S9 (and #6 note) |
| A user sweep found that NAM "doesn't have oversampling nor any kind of filtering", so harmonics fold at 24 kHz. Opinions on audibility differ. | S10 |
| The NAM LV2 README says that running at an even multiple of the model rate gives correct oversampling, at significant CPU cost. | S11 |
| AudioDSPTools issue #10 is an open enhancement request for a lower-alias resampling option. | S19 |
| Activation smoothness drives ASR in neural amp models (DAFx 2025). | S8 |

## 3. Recommendations

### 3.1 Per-block strategy (initial; validate with §3.3)

| Block | Nonlinearity | Live (44.1/48 kHz) | Live (88.2/96 kHz) | HQ / offline | Filter |
|---|---|---|---|---|---|
| Drive (soft clip, tanh/asym) | memoryless | ADAA1 + 2× | ADAA1, 1× | ADAA1 + 4× | IIR |
| Fuzz / hard clip | memoryless, corners | ADAA2 + 4× | ADAA2 + 2× | ADAA2 + 8× | IIR |
| Amp preamp stages (cascaded, high gain) | stateful (filters between stages) | 4× for the whole preamp, ADAA1 per shaper | 2× | 8× | IIR (live) / FIR (HQ) |
| Power amp / sag | mild | shares the preamp's oversampled domain, or 1× + ADAA1 | 1× | 2× | IIR |
| Compressor / gate | gain computer only | 1× (no oversampling) | 1× | 1× | n/a |
| NAM capture | learned | 1× (native rate) | 2× via dilation scaling if exact | 2× | n/a (see #6) |
| Cab IR, EQ, delay, reverb, modulation | linear | 1× | 1× | 1× | n/a |

Rules:
- **Oversample once per contiguous nonlinear section**, not per shaper. Cascaded 2× halfband stages reach 4×/8×,
  as JUCE does [S5].
- **Allocate every oversampler buffer in `prepare()`** for the maximum block size × factor. Nothing allocates in
  `process()`.
- **Change the oversampling factor only in `prepare()`, never on the audio thread.** The quality setting (Live/HQ)
  is non-RT: it triggers a re-prepare and a latency update.
- **Filter parameters:** copy JUCE's max-quality defaults (§2.2). Upsampler stopband ≥ 90 dB at the first stage,
  decimator ≥ 75 dB, transition widths 0.05/0.06 at the first stage.

### 3.2 Library verdict

| Use | Choice | Licence verdict |
|---|---|---|
| IIR halfband in `dsp-core` | Upstream HIIR v1.50 (`http://ldesoras.fr/src/hiir-1.50.zip`, SHA-256 `8a13724e8d698a40f53687d40466443879f6250abd2229097f3990b76e6855a6`, computed 2026-10-25), vendored or fetched at configure time, coefficients from its design tool | WTFPL-2.0: GPL-compatible (FSF) and redistributable. Record the licence and SHA-256 in `THIRD_PARTY.md`. |
| FIR halfband (HQ) | Own implementation: equiripple coefficients designed offline (Python `scipy.signal.remez`) and committed as constants | Our code, AGPL |
| ADAA | Own implementation in `dsp-core` (a few dozen lines per shaper). Reference: [S4], ADAA repo (BSD-3-Clause) [S20] | Our code, AGPL |
| JUCE `dsp::Oversampling` | Do not use in `dsp-core`. Allowed only as a cross-check in tests. | AGPLv3 OK, but forbidden by layering |

### 3.3 Aliasing test (to add to `tests/`)

1. **Stimulus.** For each nonlinear block and preset gain setting, at fs ∈ {44.1, 48} kHz and N = 65536 (2¹⁶), pick
   nominal f0 ∈ {110, 440, 1000, 2500, 5000, 8000} Hz. Set `k = round(f0·N/fs)`; if k is even, use k + 1. That makes k
   odd and therefore coprime with N. Play the exact frequency f0 = k·fs/N at −6 dBFS and 0 dBFS. Render 8192 + N
   samples, discard the first 8192 as warm-up, and analyse the last N.
   With odd k, the harmonics m·k (mod N) never land on DC or Nyquist, and no alias can land on a harmonic
   bin: m·k ≡ ±j·k (mod N) forces m ≡ ±j (mod N), for harmonic orders below N/2. This avoids the fs/4 and fs/6 coincidences.
2. **Spectrum.** Use a rectangular window (no window). With coherent sampling, every harmonic and alias falls exactly
   on one bin. Compute the real FFT X[b], b = 0…N/2, in float64.
3. **Bins.** Fundamental bin = k. Harmonic bins H = { m·k : m ≥ 2, m·k < N/2 }. Alias bins A = every bin with
   1 ≤ b < N/2 and b ∉ {k} ∪ H. DC (b = 0) and Nyquist (b = N/2) are excluded.
   **ASR = 10·log10( Σ_{b∈A} |X[b]|² / Σ_{b∈H} |X[b]|² )** (cf. [S8]; the fundamental is excluded).
   Also report **alias-to-fundamental = 10·log10( Σ_{b∈A} |X[b]|² / |X[k]|² )** and the worst single alias bin in dBc.
   If H is empty (f0 > fs/4) or Σ_H is below −150 dBFS, gate on alias-to-fundamental only. Noise from float32 processing
   also lands in A. That is acceptable, because it sits far below the thresholds.
4. **Proposed thresholds** (*unverified*: engineering targets, to tune against renders and [S3]'s NMR approach):
   ASR ≤ −60 dB for f0 ≤ 2.5 kHz and ≤ −40 dB for f0 ≤ 8 kHz in Live mode. HQ mode is 20 dB stricter.
   Fail CI if a change worsens any ASR by > 3 dB.
5. Add a sweep render (20 Hz → 20 kHz, 10 s) per factory preset to the golden set for visual spectrogram review.

### 3.4 Latency to report

`latency = ceil(oversampler_latency(factor, filter) + adaa_delay / factor + fir_delays)`, measured in `prepare()`
by impulse response (group delay at a 1 kHz reference) rather than by formula. Pad with an integer delay so that the
total matches the reported integer **at the reference frequency**, report it through `setLatencySamples`, and cover it
with a test (1 kHz tone burst in, expected delay ± 0.5 sample).
- **FIR halfbands:** linear phase, so the padding is exact at every frequency.
- **IIR halfbands:** the group delay depends on frequency (smallest at low frequencies, rising towards Nyquist), so
  the padding is exact only at 1 kHz. Document this.
- **Expected order of magnitude** (*unverified*; to be measured with JUCE `getLatencyInSamples()` as a cross-check in the
  first DSP ticket): IIR 2×/4× ≈ 1–5 base-rate samples; FIR 2×/4× ≈ 10–40 base-rate samples at max quality.

## 4. Open questions (for the architect)

1. Should "HQ" be a global quality switch (Live/HQ) or per-block? And should offline renders force HQ automatically
   (the host's non-realtime flag)?
2. Should the whole amp section (preamp + power amp) share one oversampled domain (one up/down pair, less filter cost
   and latency), or should each block own its oversampler (simpler modularity)?
3. Which ASR thresholds become CI gates, and which only warn? This needs listening/NMR validation (§3.3, item 4).
4. Should we train our own NAM captures with smoother activations [S8] to reduce aliasing at source? This ties to the
   capture question in #6.
5. IIR halfband phase distortion near Nyquist: acceptable in parallel/dry-wet blends? If not, mixes need the FIR path.

## 5. Sources (all accessed 2026-10-25)

- S1: J. D. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution", DAFx-16, Brno, 2016. https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA (PDF: https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf)
- S2: S. Bilbao, F. Esqueda, J. D. Parker, V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", IEEE Signal Processing Letters 24(7):1049–1053, 2017. https://aaltodoc.aalto.fi/items/470aab15-1702-4ccf-a148-24e6173079fb
- S3: J. Kahles, F. Esqueda, V. Välimäki, "Oversampling for Nonlinear Waveshaping: Choosing the Right Filters", JAES 67(6):440–449, 2019, doi:10.17743/jaes.2019.0012. https://acris.aalto.fi/ws/portalfiles/portal/34665796/ELEC_Kahles2019_Oversampling_JAES.pdf
- S4: J. Chowdhury, ADAA notebook (CCRMA). https://ccrma.stanford.edu/~jatin/Notebooks/adaa.html
- S5: JUCE 9.0.4, `modules/juce_dsp/processors/juce_Oversampling.h/.cpp`; JUCE `LICENSE.md` (AGPLv3 / commercial). https://github.com/juce-framework/JUCE/tree/9.0.4/modules/juce_dsp/processors
- S6: L. de Soras, HIIR v1.50: product page, plus `license.txt` (WTFPL Version 2) and headers inside `hiir-1.50.zip`. http://ldesoras.fr/prod.html ; http://ldesoras.fr/src/hiir-1.50.zip
- S7: HIIR header-only mirror (third-party fork, WTFPL, last push 2021-11-01). https://github.com/unevens/hiir
- S8: R. Sato, J. O. Smith III, "Aliasing Reduction in Neural Amp Modeling by Smoothing Activations", DAFx 2025, arXiv:2505.04082. https://arxiv.org/abs/2505.04082
- S9: NeuralAmpModelerPlugin, `NeuralAmpModeler.h` (`ResamplingNAM`). https://github.com/sdatkinson/NeuralAmpModelerPlugin
- S10: The Gear Forum, "NAM: Neural Amp Modeler" thread (user sweep analysis). https://thegearforum.com/threads/nam-neural-amp-modeler.1698/page-52
- S11: neural-amp-modeler-lv2 README. https://github.com/mikeoliphant/neural-amp-modeler-lv2
- S12: ADR-0001 (this repo), `docs/adr/0001-license-and-framework.md`.
- S13: M. Holters, "Antiderivative Antialiasing for Stateful Systems", DAFx-19; extended in Applied Sciences 10(1):20, 2020, doi:10.3390/app10010020. https://www.mdpi.com/2076-3417/10/1/20
- S14: F. Esqueda, S. Bilbao, V. Välimäki, "Aliasing Reduction in Clipped Signals", IEEE Trans. Signal Processing 64(20):5255–5267, 2016, doi:10.1109/TSP.2016.2585091. https://www.pure.ed.ac.uk/ws/files/26997332/07499828.pdf
- S15: F. Esqueda, V. Välimäki, S. Bilbao, "Rounding Corners with BLAMP", DAFx-16. https://dafx.de/paper-archive/details/xRM0GiuF-JmeN0_aaP9VNg
- S16: FSF, "Various Licenses and Comments about Them": WTFPL Version 2 is listed under GPL-compatible free software licences ("a lax permissive non-copyleft free software license, compatible with the GNU GPL"). https://www.gnu.org/licenses/license-list.en.html#WTFPL
- S17: r8brain-free-src (MIT). https://github.com/avaneev/r8brain-free-src
- S18: chowdsp_utils README (per-module licences). https://github.com/Chowdhury-DSP/chowdsp_utils
- S19: AudioDSPTools issue #10 (lower-alias resampling request). https://github.com/sdatkinson/AudioDSPTools/issues/10
- S20: J. Chowdhury, ADAA experiments (BSD-3-Clause). https://github.com/jatinchowdhury18/ADAA
