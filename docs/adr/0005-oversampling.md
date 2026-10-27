# ADR-0005: Oversampling and anti-aliasing strategy

- Status: **Accepted** (2026-10-25; cross-account review approved and merged in Phase 2).
- Deciders: architect (acct1/Sable). Issue #27.
- Rests on: [`docs/research/oversampling.md`](../research/oversampling.md) (#10, cited as OS §x),
  [`docs/research/nam-runtime.md`](../research/nam-runtime.md) (NR §x), ADR-0001 (no JUCE in `dsp-core`),
  ADR-0002 (#26: NAM runtime, fixed amp-slot latency), ADR-0004 (#37: threading), ADR-0006 (HIIR pin), ADR-0007 (the aliasing harness; ADR-0005 owns the
  nonlinear thresholds).

## Context

Waveshapers alias, and the effect is worst for high-gain and hard-clip stages at 44.1/48 kHz. The research shows:
- ADAA plus modest oversampling beats oversampling alone (OS §1.1).
- Polyphase IIR halfbands give low latency, while FIR halfbands give linear phase with more latency (OS §1.3).
- ADAA adds 0.5 sample (1st order) or 1 sample (2nd order) of delay, plus a high-frequency roll-off (OS §1.5).
- `dsp-core` cannot use `juce::dsp::Oversampling` (ADR-0001).
- HIIR 1.50 (WTFPL-2.0, FSF GPL-compatible) is pinned in ADR-0006.
- NAM models run without internal oversampling (OS §2.3).
- ADR-0007 defines a coherent odd-bin, rectangular-window ASR harness and leaves the nonlinear thresholds to this ADR.

## Decision

### D1. Building blocks in `dsp-core/oversampling/`
- **`Oversampler<N>`** (N ∈ {1, 2, 4, 8}) is a cascade of 2× halfband stages (OS §3.1). The filter kind is chosen in
  `prepare()`:
  - **IIR** (default, Live): HIIR two-path polyphase allpass halfband (`hiir::Upsampler2x*` / `Downsampler2x*`, SIMD
    variant chosen at compile time). Coefficients are designed offline with HIIR's designer to the specs in D3 and
    committed as `constexpr` arrays.
  - **FIR** (HQ): our own equiripple halfband, with coefficients from `scipy.signal.remez`. The coefficients and the
    script are committed under `tools/design/halfband_fir.py`.
- **`Adaa1<Shape>` / `Adaa2<Shape>`**: templated on a shape that provides `f`, `F1` (and `F2`). Use the
  ill-conditioning fallback `f((x[n] + x[n−1]) / 2)` when `|Δx| < 1e-5` (float) (OS §2.1).
  `tanh`'s `F1 = log(cosh x)` uses the overflow-safe form `|x| + log1p(exp(−2|x|)) − log 2`.
- Nothing allocates in `process()`. Buffers are sized in `prepare()` for maxBlock × N. The factor and filter kind
  change only in `prepare()`.

### D2. Per-block factors (from OS §3.1; the thresholds in D5 make them binding)

Oversampled regions follow `docs/spec/signal-flow.md` (#52):

| Region | Blocks | Live 44.1/48 kHz | Live 88.2/96 kHz | HQ (any rate) | Filter | ADAA |
|---|---|---|---|---|---|---|
| **OS-1** | Drive A → Drive B (adjacent, every voicing) | 4× | 2× | 8× | IIR (Live), FIR (HQ) | Soft-clip voicings: 1st order. Fuzz/hard-clip voicings: 2nd order |
| **OS-2** | Classic amp (preamp → tone stack → power amp) | 4× | 2× | 8× | IIR (Live), FIR (HQ) | 1st order per stage |
| none | Compressor, gate, EQ, cab IR, modulation, delay, reverb, doubler | 1× | 1× | 1× | n/a | none. Exception: saturating delay/tape modes run 2× + ADAA1 on the wet feedback path only. Their oversampler latency is absorbed by shortening the delay line, so `L_dly` = 0 (signal-flow #52), which requires the minimum delay time to be ≥ that latency. The dry path is untouched |
| none | NAM amp | 1× (native rate, ADR-0002 D4) | 1× | 1× in v1 (see D7) | n/a | n/a |

- **OS open question 2 (shared domain), decided by the orchestrator:**
  - **OS-1** is one up/down pair shared by the adjacent Drive A + Drive B pair. It runs at the **hungriest voicing's
    factor** (fuzz/hard clip: 4× Live at 44.1/48 kHz) for every voicing, so a voicing change never changes the factor
    or the latency. The latency is constant without padding: one pair and one fixed ADAA delay (`a_max` = 1 sample at
    the oversampled rate).
  - **OS-2** is the classic amp's own region. It stays separate from OS-1 because NAM channels sit in the same amp slot
    and must not be forced into OS-2 (signal-flow §OS regions).
  - The clean DI blend (`amp.cleanBlendPct`, signal-flow #52) is delayed by the integer `L_slot + L_cab`, so the
    total latency is unchanged. Against an IIR amp path, the residual fraction (≤ 0.5 sample, D4) remains between the
    blended signals. This is accepted, because the amp path is nonlinear and no null applies.
  - The cost is that soft-clip voicings in OS-1 run at 4× rather than the 2× the research suggests (OS §3.1). The
    extra CPU is accepted for one pair and constant latency.

### D3. Filter specifications
| Stage | IIR (Live) | FIR (HQ) |
|---|---|---|
| Upsampler, first 2× stage (fs ↔ 2fs) | stopband ≥ 90 dB, transition 0.05 | stopband ≥ 100 dB, transition **0.03** (Amendment A2; was 0.05) |
| Decimator, first 2× stage (fs ↔ 2fs) | stopband ≥ 75 dB, transition **0.01** (Amendment A1; was 0.06) | stopband ≥ 100 dB, transition **0.03** (Amendment A2; was 0.06) |
| Later stages | as JUCE: +10 dB stopband per stage; transition 0.10 (up) and 0.12 (down), i.e. × 2 the JUCE stage-0 values | stopband ≥ 100 dB, transition 0.06 (× 2 the A2 stage-0 value) |

Transition is relative to the stage's higher rate. Apart from Amendments A1 and A2, these are JUCE's max-quality defaults
(OS §2.2), and our CI measures them (REQ-OS-002). Passband ripple must be
≤ 0.1 dB up to 20 kHz at 44.1/48 kHz, which the IIR satisfies by construction (allpass halfband) and the FIR must be
designed to.

### D4. Latency
- Each block **measures** its delay `D` in `prepare()`: the 1 kHz group delay of oversampler + ADAA, in base-rate
  samples (OS §3.4). It reports `getLatencySamples()` = `round(D)`, the nearest integer.
- **FIR:** the halfband design (odd tap count per stage) and ADAA compensation are chosen so that `D` is an integer.
  The block is then exactly a pure delay of `round(D)` at every frequency, so null tests apply.
- **IIR:** `D` is fractional and frequency-dependent (non-linear phase). The **residual fraction** `D − round(D)`
  (|·| ≤ 0.5 sample) is accepted and recorded per block and factor in the test log. Above and below 1 kHz, the phase
  deviation is part of the block's sound and is covered by goldens. IIR blocks are therefore tested by magnitude and
  group delay, **not** by a null against an integer-delayed input (REQ-OS-001b).
- **Internal dry/wet blends** (for example a drive "blend" knob) send the dry path through the **same** up/down
  chain with identity processing, so dry and wet stay phase-matched with either filter kind (this answers OS open
  question 5). Parallel blends at the graph level are compensated by the engine's latency alignment
  (`docs/spec/threading.md` and the signal-flow spec).
- **Constant latency per (host rate, quality mode)** (REQ-303): each chain slot reports a fixed integer latency, computed in
  `prepare()` as the maximum over the block types that slot can hold. Shorter blocks, and **bypassed** blocks, are
  padded with a preallocated delay. Enabling, bypassing or swapping a block never changes the reported total, and
  crossfades are time-aligned. The amp slot uses ADR-0002 D4's `L_slot`, which includes the classic amp's oversampler
  latency from this ADR (`L_classic`).
- The engine sums the slot latencies and reports the total through `setLatencySamples`, from the control thread, only
  in `prepare()` (a Live/HQ change or SR/block change).
- Expected magnitude (*unverified*, OS §3.4): IIR 2×/4× ≈ 1–5 samples, FIR 2×/4× ≈ 10–40 samples per block. Live mode
  must stay at **≤ 16 samples total** for the full chain at 48 kHz (REQ-302, confirmed here), excluding the
  NAM resampler, which is 0 at 48 kHz (REQ-OS-005).

### D5. Aliasing thresholds (ADR-0007 harness; ASR excludes the fundamental)
| Mode | f0 ≤ 2.5 kHz | 2.5 < f0 ≤ 8 kHz | Gate |
|---|---|---|---|
| Live | ASR ≤ −60 dB | ASR ≤ −40 dB | **CI fail** |
| HQ | ASR ≤ −80 dB | ASR ≤ −60 dB | **CI warn only** in v1. It becomes a gate after the first full set of HQ renders (owner: qa-engineer, Phase 3 exit) |
| Any mode | an ASR worse than the last `main` value by > 3 dB at any (f0, level, fs) | | **CI fail** |

- When H is empty (f0 > fs/4) or below −150 dBFS, gate on alias-to-fundamental with the same numbers (OS §3.3).
- Measured at each block's **default** parameters and at max drive/gain. The values are *unverified* engineering
  targets (OS §3.3). This answers OS open question 3: Live gates, and HQ warns until it is calibrated.

### D6. Quality mode (OS open question 1)
- A **global** app/plugin setting **"Processing quality: Live / HQ"**, stored in app settings and plugin state, not
  in presets. Changing it is non-RT: it re-prepares the graph on the message thread and updates the reported latency.
- **No automatic switch on offline render in v1 inside hosts**, because changing the reported latency mid-session is
  unreliable across hosts (*unverified*). Revisit after pluginval and host tests (owner: audio-io-engineer, Phase 4).
- **`osp-render` (our own CLI) may force HQ** with `--quality hq`, because it prepares the engine itself and has no
  host latency contract. Goldens are rendered in both modes (Consequences). `requirements.md` REQ-421 ("offline renders
  use HQ") is to be reworded to cover `osp-render` and hosts' user setting only (owner: acct3/Moth, as requirements
  author).

### D7. NAM and aliasing
- v1 runs NAM at the native rate (ADR-0002 D4). A 2× "HQ" NAM via native SR adaptation is **deferred** until
  REQ-NAM-004 reports which models support it (owner: dsp-engineer). Its CPU (≈ 2×) must also fit ADR-0002 D7.
- Training our own captures with smoother activations (OS open question 4; OS source S8) is **deferred** to the own-captures
  ticket (ADR-0002 D8; owner: preset-designer + dsp-engineer).

## Amendment A1 (2026-10-26, #98): stage-0 IIR decimator transition 0.06 → 0.01

- Status: **Accepted** (2026-10-26, reviewed by acct2). Author: acct1/dsp-engineer (Sable). Evidence from #67 (drive).
- **Problem.** With transition 0.06, the stage-0 decimator only reaches its stopband at 0.62·fs. Harmonics that a
  nonlinear block creates between fs/2 and 0.62·fs fold back to between 0.38·fs and fs/2 with little rejection. At
  44.1/48 kHz that is 16.8–24 kHz. They dominate the D5 ASR. Measured on the #67 drive stage at 4× (all four voicings,
  0 dBFS), counting aliases over all bins as ADR-0007 does:
  - 1 kHz at max drive: ASR −48.3…−55.3 dB (gate −60). The worst alias is at 21.1 kHz (44.1k) / 23.0 kHz (48k),
    from H23/H25 just above Nyquist.
  - 5 kHz @ 48 kHz: ASR −28.3…−33.5 dB (gate −40) for every voicing and drive. H5 at 25.0 kHz folds to 23.0 kHz.
  - The same renders counting only aliases ≤ 20 kHz: −62…−104 dB. So the failures are the transition band, not ADAA.
- **Decision.** Design the stage-0 IIR decimator for **75 dB with transition 0.01**. HIIR then gives 9 coefficients
  instead of 6, and the stopband starts at 0.52·fs. Leftover transition aliases fold to 0.48–0.5·fs (≥ 21.2 kHz at
  44.1k, ≥ 23.0 kHz at 48k). The upsampler, the later stages and the FIR (HQ) column are unchanged. This stays an IIR
  halfband, so the D4 latency rules are unchanged.
- **Cost (measured, #98 PR, VS 2022 Release):**
  - Latency: unchanged after rounding. The 1 kHz delay D at 48 kHz goes from 2.643/3.948/4.729 to 2.666/3.970/4.751
    (2×/4×/8×), so `round(D)` stays 3/4/5. Live 4× stays at 4 samples per region (REQ-302).
  - CPU: `oversampler_iir_4x` is 0.097 % of a core, against 0.098 % before, i.e. within noise.
  - The slowest pole moves to √0.984 per 2fs sample. The 1024-sample identity IR used for D in `prepare()` still
    decays below −100 dB, and D matches the test's 4096-sample IR within 1e-3.
  - Measured stage-0 decimator stopband: −78.0 dB (spec 75).
- **Result on #67 with A1** (all 64 cases: 4 voicings, 44.1/48 kHz, −6/0 dBFS, default/max drive, 1 and 5 kHz):
  - Worst ASR −74.3 dB against the −60 gate (fuzz, 1 kHz, max drive, 0 dBFS, 44.1 kHz).
  - Worst ASR −57.2 dB against the −40 gate (dist, 5 kHz, max drive, −6 dBFS, 44.1 kHz).
  - The D5 gates are unchanged.
- **Not chosen:**
  - (a) Restricting the ASR alias set to ≤ 20 kHz: that changes ADR-0007's harness and loosens the gate.
  - (b) A steep in-domain anti-alias low-pass before decimation: it needs a per-rate elliptic design, more CPU, and
    more phase shift for the same effect.
  - (c) Transition 0.005: 10 coefficients and a pole at √0.992, for 0.01·fs more alias-free band. It can be revisited if
    a block needs it.
- **HQ (FIR):** the FIR decimator column keeps 0.06 for now. HQ ASR is warn-only in v1 (D5). #63 part 3 reports the
  HQ ASR with the FIR, and proposes the matching FIR transition if needed (FIR taps, and so latency, grow as
  1/transition).

## Amendment A2 (2026-10-26, #63 part 3): FIR (HQ) stage-0 transition 0.05/0.06 → 0.03

- Status: **Accepted** (2026-10-26, reviewed by acct2). Author: acct1/dsp-engineer (Sable).
- **Problem.** D3 itself requires FIR passband ripple ≤ 0.1 dB up to 20 kHz at 44.1/48 kHz. An equiripple FIR halfband
  with transition tw only holds its passband to (0.25 − tw)·2fs and then rolls off to −6 dB at fs/2. With D3's 0.05
  (up) / 0.06 (down), the passband ends at 17.6 kHz at 44.1 kHz, and 20 kHz loses −0.57 dB (up) − 0.98 dB (down)
  ≈ **−1.5 dB** (prototype, scipy remez). The IIR column does not have this problem, because an elliptic halfband stays
  flat well into its transition band. So the FIR column contradicts D3's own ripple clause.
- **Decision.** Stage 0 (up and down): transition **0.03**, which puts the passband edge at 0.44·fs = 19.4 kHz at
  44.1 kHz. Later stages: 0.06 (× 2). At 44.1 kHz, 20 kHz still lies in the transition band (19.4–22.05 kHz), so
  compliance with the 20 kHz clause is by measurement (below), not by construction. Every FIR filter is **designed for 110 dB**. That is above the D3 minimum
  of 100 dB, because in a halfband the passband ripple equals the stopband ripple, and REQ-OS-001a's −120 dBFS identity
  null needs it. `tools/design/halfband_fir.py` picks the tap counts so the up + down delay of each stage is an
  integer at the base rate (D4).
- **Measured (#63 part 3 PR, VS 2022 Release):**
  - Stopbands: stage 0 −112.4 dB up / −112.5 dB down; stage 1 −123.2 / −116.4; stage 2 −130.3 / −123.2.
  - Passband ripple to 20 kHz: ≤ 0.037 dB at 44.1 kHz and ≤ 1e-4 dB at 48 kHz and above.
  - Identity null, 64 tones ≤ 18 kHz at −12 dBFS RMS, against the input delayed by the latency: −121.2…−122.3 dBFS RMS
    at every rate and factor (gate −120).
  - **Latency** is an exact integer: **57 / 72 / 80 base samples** at 2× / 4× / 8×. HQ at 8× is 1.67 ms at 48 kHz per
    oversampled region. That is above the *unverified* 10–40 samples D4 estimated, and it is the cost of the 20 kHz
    passband. HQ is not the Live mode (REQ-302 applies to Live only), and the latency is reported to the host in
    `prepare()` (D6). A whole HQ chain with both oversampled regions (OS-1 and OS-2) is about **160 samples**
    (3.3 ms at 48 kHz) of oversampler latency, before ADAA and other blocks.
- **Not chosen:** keeping 0.05/0.06 (about 31 samples at stage 0) breaks the ripple clause by 1.4 dB; tw 0.02 (75
  samples at stage 0) gains nothing at 20 kHz over 0.03.
- **Follow-up:** with 2 ADAA slots of 1 sample at 8×, an FIR OS-1 region has D = 80 + 2/8. D4 asks for an integer, so
  `DriveRegion` pads 6 samples at the oversampled rate when it switches HQ to the FIR kind (after #104).

## Requirements

| ID | Requirement | Test |
|---|---|---|
| REQ-OS-001a | **FIR** `Oversampler<N>` (N = 2, 4, 8) with identity processing: the measured delay is an integer, and the output nulls against the input delayed by `getLatencySamples()` to ≤ −120 dBFS RMS (broadband noise band-limited to 18 kHz at 44.1/48 kHz). | `tests/dsp/oversampling` |
| REQ-OS-001b | **IIR** `Oversampler<N>` (N = 2, 4, 8) with identity processing: passband magnitude within ±0.1 dB up to 20 kHz, stopband per D3 (both from the impulse response FFT); measured 1 kHz group delay within ±0.5 sample of `getLatencySamples()`. The residual fraction is logged. No null test. | `tests/dsp/oversampling` |
| REQ-OS-002 | Measured stopband and passband per stage meet D3 (impulse response → FFT). | `tests/dsp/oversampling` |
| REQ-OS-003 | Every nonlinear block meets the D5 Live thresholds at 44.1 and 48 kHz, at −6 and 0 dBFS, at default and maximum gain. | ADR-0007 harness |
| REQ-OS-004 | `Adaa1`/`Adaa2` match the closed-form linear-case kernels for `f(x) = x` (0.5 / 1 sample of delay), and stay finite for a slowly varying input (Δx < 1e-7) and for ±1e6 input. | `tests/dsp/adaa` |
| REQ-OS-005 | The reported latency equals the measured 1 kHz delay ± 0.5 sample for every block and mode. The full chain in Live mode is ≤ 16 samples at 48 kHz (REQ-302). Toggling the bypass of each block, and swapping the amp, leaves the reported latency and the measured 1 kHz delay unchanged (REQ-303). | `tests/dsp/latency` |
| REQ-OS-006 | Changing the factor or quality never happens in `process()`. The RT checker (ADR-0007) sees no allocation while the quality toggles 100× with audio running. | RT checker |
| REQ-OS-007 | A block with an internal dry/wet blend and identity wet processing gives the same output at blend 0%, 50% and 100%, to ≤ −120 dBFS RMS (dry and wet share the same up/down chain, so this holds for IIR and FIR alike). | `tests/dsp/blend` |

## Consequences
- + Aliasing is controlled with measurable gates, and the CPU stays low (2–4× instead of 8–16× without ADAA).
- + HIIR's SIMD halfbands keep the Live latency to a few samples.
- − ADAA needs closed-form antiderivatives, so each shaper ships `f`, `F1` (and `F2`). Shapers without one must use
  oversampling alone, at a higher factor, and must still meet REQ-OS-003.
- − IIR phase shift near Nyquist is part of the Live sound, so goldens differ between Live and HQ (two golden sets
  for the nonlinear presets).
- − The HQ setting changes the reported latency, and hosts must re-query it.

## Deferred items
| Item | Owner | When |
|---|---|---|
| HQ thresholds become gates | qa-engineer | Phase 3 exit |
| Offline auto-HQ | audio-io-engineer | Phase 4 (after host tests) |
| NAM 2× HQ | dsp-engineer | after REQ-NAM-004 |
| Smoother-activation own captures | preset-designer + dsp-engineer | own-captures ticket |
| Stateful ADAA (Holters) for diode-in-filter circuits | dsp-engineer | only if a block fails REQ-OS-003 with D2 factors |
