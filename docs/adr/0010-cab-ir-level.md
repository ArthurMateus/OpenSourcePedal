# ADR-0010: Cab IR level normalisation at load

- **Status:** Accepted (2026-10-27)
- **Deciders:** `default/dsp-engineer` (issue #158). Owner rule in force: never ask, decide and record.
- **Inputs:** issue #158; QA pass 3 on #95 (comment 6454547977) and pass 2 (comment 6454832882);
  [`parameters.md`](../spec/parameters.md) §3.1 (`cab.*`); [`signal-flow.md`](../spec/signal-flow.md) §5 (the linear
  enable crossfade assumes processed ≈ dry level); [ADR-0004](0004-threading.md) §4 (IRs are built on the worker).

## Context

The bundled cab IRs (`assets/cab/*.wav`) are peak-normalised, as most published cab IRs are. Nothing compensated
for it, so the cab added gain: QA measured +8.6 dB on the DI through the engine (pass 3), +12.4 … +16.3 dB on the DI
for the bare block (pass 2), and a 440 Hz sine stepped by 11.75 dB when `cab.enabled` was toggled. With the cab on,
the output limiter sat at its ceiling. The level of a peak-normalised IR says nothing about its loudness: it depends
on how narrow its main pulse is. User IRs come at any level.

## Decision

### D1. Every IR is normalised at load to 0 dB pink-noise gain over 40 Hz – 20 kHz

`IrLoader::condition()` (the worker-side step that resamples and caps an IR, ADR-0004 §4) scales the final taps, at
the processing rate, by

    g = 1 / sqrt( Σ_k |H(f_k)|² / f_k  /  Σ_k 1 / f_k ),   40 Hz ≤ f_k ≤ min(20 kHz, 0.45·fs)

over the bins of a zero-padded FFT (≤ 1 Hz bins). Pink noise has a power density proportional to 1/f (equal power per
octave), so this ratio is the power gain of the IR for band-limited pink noise, and after scaling, pink noise in that
band leaves the cab at the level it went in. The rule applies to factory and user IRs alike.

- **Why pink:** pink noise has equal energy per octave, which is the usual stand-in for the long-term spectrum of
  music and the standard signal for level calibration (Wikipedia, "Pink noise", https://en.wikipedia.org/wiki/Pink_noise,
  accessed 2026-10-27). A guitar's energy is spread over octaves, not over hertz, so white-noise (unit-energy)
  normalisation over-weights the empty 5–20 kHz region where a speaker radiates almost nothing.
- **Why 40 Hz:** below it a guitar cab passes nearly nothing and a guitar plays nothing (an 8-string's low F♯ is
  46 Hz), but pink weighting would give the 20–40 Hz octave a tenth of the total weight. Including it lifts every IR
  by about 0.4 dB and pushes the guitar-band error over 2 dB (table below).
- **Why 20 kHz:** the audio band. A cab passes little above 10 kHz, so the upper edge matters little (moving it from
  10 to 20 kHz changes the gain of the bundled IRs by about 0.5 dB).
- **Measured** (48 kHz, the 7 bundled IRs, level with the cab on minus the cab off, Python check and the C++ tests):

| Law | DI `strum-arp-01` | pink 80 Hz–5 kHz | pink 40 Hz–20 kHz |
|---|---|---|---|
| none (as shipped) | +12.4 … +16.3 dB | +13.9 … +19.3 dB | +12.3 … +17.8 dB |
| unit energy (Σh² = 1) | +2.9 … +7.3 | +5.7 … +7.6 | — |
| **pink 40 Hz–20 kHz (chosen)** | **−1.48 … +1.30** | **+1.5 … +1.7** | **0.00** |
| pink 20 Hz–20 kHz | −1.13 … +1.76 | +1.83 … +2.06 | — |
| pink 80 Hz–5 kHz | −3.00 … −0.39 | 0.00 | — |
| K-weighted pink 20 Hz–20 kHz (ITU-R BS.1770 weighting) | −1.31 … +2.15 | +1.65 … +2.45 | — |

  The DI spread (about 2.8 dB) is the same for every fixed-spectrum law: it comes from how much of a clean DI's low
  end each cab passes. The chosen band centres the DI on 0 dB and keeps a guitar-band signal within +1.7 dB.

### D2. Identity and edge cases

- A unit impulse (the engine's "no cab" IR, #95) has |H| = 1 in every bin, so both sums are built from the same terms
  and g = 1 exactly: it stays bit-exact (test).
- A correction within ±0.01 dB is not applied, so normalising an already normalised IR changes nothing.
- The correction is clamped to ±40 dB, so a near-silent file is not lifted into noise. An all-zero IR stays silent.
- The work is on the worker (one double FFT of ≤ 2^17 points per IR); the audio thread is unchanged.

### D3. Keeping an IR's own level is a loader option, not a v1 parameter

`IrLoader::condition(…, IrLevel::Keep)` skips the step (tests and tools). The spec has no rule that user IRs keep
their own level, so v1 normalises every IR, and `cab.ch<N>.irK.levelDb` (−60 … +6 dB) stays the user's trim. A
preset-level "keep the file's level" switch can be added later as a preset-only `cab` field (an ADR-0003 schema
change); it is not needed to fix #158.

## Consequences

- \+ Enabling the cab, or swapping between any two bundled IRs, moves the DI level by at most 1.5 dB and the spread
  between IRs is 2.8 dB (was +12 … +16 dB on enable). Pink noise in the reference band is unchanged to 0.02 dB.
- \+ The same law holds at 44.1/48/88.2/96 kHz because it runs on the taps the convolver will use.
- \+ User IRs at any level behave the same as factory ones.
- − A pure tone still steps by the cab's response at that frequency (a 440 Hz sine: −0.94 … +0.90 dB across the set at
  48 kHz, was +11.35 … +17.52 dB). A tone is not a loudness reference.
- − Tone-match (`osp-tonematch`, ADR-0008) and any preset or golden made before this change see the cab 12–18 dB
  quieter. No factory presets or goldens exist yet (#158 was filed to land first).

## Alternatives considered

- **Unit energy (Σh² = 1):** simple, but weighted to white noise; the bundled cabs still add +2.9 … +7.3 dB on the DI.
- **A fixed trim** (for example the −18 dB that the NAM IR loader applies, `dsp/ImpulseResponse.cpp` in
  https://github.com/sdatkinson/AudioDSPTools, MIT, accessed 2026-10-27): assumes every file is peak-normalised;
  wrong for user IRs at other levels, and the bundled set would still spread by 5.5 dB.
- **Per-IR `levelDb` in an asset manifest or re-rendered assets:** fixes only the factory IRs, not user IRs.
- **Loudness weighting (ITU-R BS.1770 K-filter, https://www.itu.int/rec/R-REC-BS.1770, accessed 2026-10-27):** the
  high-shelf rewards the 2–5 kHz cone peaks; on the DI it did slightly worse than plain pink (table).
- **Normalising to the guitar band only (80 Hz–5 kHz):** exact for that band, but leaves the DI −3.0 dB low on the
  1×12.
