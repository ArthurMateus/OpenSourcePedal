# ADR-0005: Oversampling and anti-aliasing strategy

- Status: **Proposed** (2026-10-25). It becomes Accepted when the PR merges.
- Deciders: architect (acct1/Sable). Issue #27.
- Rests on: [`docs/research/oversampling.md`](../research/oversampling.md) (#10, cited as OS §x),
  [`docs/research/nam-runtime.md`](../research/nam-runtime.md) (NR §x), ADR-0001 (no JUCE in `dsp-core`),
  ADR-0002 (#26, in review: NAM runtime), ADR-0006 (HIIR pin), ADR-0007 (the aliasing harness; ADR-0005 owns the
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

| Block | Live 44.1/48 kHz | Live 88.2/96 kHz | HQ (any rate) | ADAA |
|---|---|---|---|---|
| Drive / boost (soft clip) | 2× | 1× | 2× Live factor | 1st order |
| Fuzz / hard clip | 4× | 2× | 2× Live factor | 2nd order |
| Classic amp (preamp → tone stack → power amp) | 4×, **one shared domain** per amp | 2× | 2× Live factor (max 8×) | 1st order per stage |
| Compressor, gate, EQ, cab IR, modulation, delay, reverb | 1× | 1× | 1× | none (except saturating delay/tape modes: 2× + ADAA1) |
| NAM amp | 1× (native rate, ADR-0002 D4) | 1× | 1× in v1 (see D7) | n/a |

- **OS open question 2 (shared domain):** each classic amp model owns **one** up/down pair around its whole
  preamp → tone stack → power-amp chain, because the tone stack between stages is cheap and runs fine at the high rate.
  Pedals each own their own oversampler, which keeps the blocks modular. Two adjacent oversampled pedals cost two
  up/down pairs. We accept that.

### D3. Filter specifications
| Stage | IIR (Live) | FIR (HQ) |
|---|---|---|
| Upsampler, first 2× stage (highest rate) | stopband ≥ 90 dB, transition 0.05 | stopband ≥ 100 dB, transition 0.05 |
| Decimator, first 2× stage | stopband ≥ 75 dB, transition 0.06 | stopband ≥ 100 dB, transition 0.06 |
| Later stages | as JUCE: +10 dB stopband per stage, transition × 2 | stopband ≥ 100 dB, transition × 2 |

These are JUCE's max-quality defaults (OS §2.2), and our CI measures them (REQ-OS-002). Passband ripple must be
≤ 0.1 dB up to 20 kHz at 44.1/48 kHz, which the IIR satisfies by construction (allpass halfband) and the FIR must be
designed to.

### D4. Latency
- Each block reports `getLatencySamples()` = `ceil(oversamplerDelay + adaaDelay / N)` in base-rate samples. It is
  **measured in `prepare()`** with a 1 kHz tone burst (group delay at the 1 kHz reference) and padded with an integer
  delay so that the total equals the reported integer at 1 kHz (OS §3.4).
- **FIR:** exact at all frequencies. **IIR:** exact only at 1 kHz. Above that, the residual phase is part of the
  block's sound and is covered by goldens.
- **Internal dry/wet blends** (for example a drive "blend" knob) send the dry path through the **same** up/down
  chain with identity processing, so dry and wet stay phase-matched with either filter kind (this answers OS open
  question 5). Parallel blends at the graph level are compensated by the engine's latency alignment
  (`docs/spec/threading.md` and the signal-flow spec).
- The engine sums block latencies along the chain and reports the total through `setLatencySamples` (ADR-0002 adds the
  NAM resampler latency).
- Expected magnitude (*unverified*, OS §3.4): IIR 2×/4× ≈ 1–5 samples, FIR 2×/4× ≈ 10–40 samples per block. Live mode
  must stay at **≤ 8 samples total at 48 kHz** for the default preset chain (REQ-OS-005).

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
- **No automatic switch on offline render in v1**, because changing the reported latency mid-session is unreliable
  across hosts (*unverified*). Revisit after pluginval and host tests (owner: audio-io-engineer, Phase 4).

### D7. NAM and aliasing
- v1 runs NAM at the native rate (ADR-0002 D4). A 2× "HQ" NAM via native SR adaptation is **deferred** until
  REQ-NAM-004 reports which models support it (owner: dsp-engineer). Its CPU (≈ 2×) must also fit ADR-0002 D7.
- Training our own captures with smoother activations (OS open question 4; OS source S8) is **deferred** to the own-captures
  ticket (ADR-0002 D8; owner: preset-designer + dsp-engineer).

## Requirements

| ID | Requirement | Test |
|---|---|---|
| REQ-OS-001 | `Oversampler<N>` (IIR and FIR, N = 2, 4, 8) with identity processing nulls against the input delayed by `getLatencySamples()` to ≤ −90 dBFS RMS (IIR, measured on a 1 kHz sine) and ≤ −120 dBFS (FIR, broadband noise band-limited to 18 kHz). | `tests/dsp/oversampling` |
| REQ-OS-002 | Measured stopband and passband per stage meet D3 (impulse response → FFT). | `tests/dsp/oversampling` |
| REQ-OS-003 | Every nonlinear block meets the D5 Live thresholds at 44.1 and 48 kHz, at −6 and 0 dBFS, at default and maximum gain. | ADR-0007 harness |
| REQ-OS-004 | `Adaa1`/`Adaa2` match the closed-form linear-case kernels for `f(x) = x` (0.5 / 1 sample of delay), and stay finite for a slowly varying input (Δx < 1e-7) and for ±1e6 input. | `tests/dsp/adaa` |
| REQ-OS-005 | The reported latency equals the measured 1 kHz delay ± 0.5 sample for every block and mode. The default preset chain in Live mode is ≤ 8 samples at 48 kHz. | `tests/dsp/latency` |
| REQ-OS-006 | Changing the factor or quality never happens in `process()`. The RT checker (ADR-0007) sees no allocation while the quality toggles 100× with audio running. | RT checker |
| REQ-OS-007 | A block with an internal dry/wet blend at 50% and identity processing nulls against its dry input (delayed by the reported latency) to ≤ −90 dBFS. | `tests/dsp/blend` |

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
