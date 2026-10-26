# ADR-0008: Tone match (reference recording + the player's DI → preset)

- **Status:** Proposed (2026-10-26)
- **Deciders:** owner request (feature), `default/dsp-engineer`
- **Inputs:** [`signal-flow.md`](../spec/signal-flow.md), [`parameters.md`](../spec/parameters.md) §2.4, §3.1, §3.2,
  [ADR-0003](0003-preset-schema.md), [ADR-0006](0006-build-and-dependencies.md), [ADR-0007](0007-test-strategy.md)

## Context

The owner asked for a feature that takes a song (an MP3/WAV, or a YouTube link) and gives the player "the guitar
tone from the video, based on your guitar". A recording only shows the *result* of player + guitar + rig + mix +
master, and the player's guitar differs from the one on the record. So the useful output is not an inverse filter
(that would bake the recorded guitar's pickups into the preset), but a setting of *our* chain that, fed with *this
player's* DI, lands near the reference.

## Decision

### D1. Match by synthesis with the real blocks

`osp::engine::tonematch` (`engine/include/osp/engine/ToneMatch.h`) renders the player's DI through the real
dsp-core blocks (`DriveRegion` slot 1, `Cab` slot 1, `Eq` graphic9 + cuts) and compares profiles. Every result is
therefore a plain preset (ADR-0003) the plugin reproduces exactly, and every intermediate step is measurable.

### D2. Profile = LTAS on a 1/4-octave grid + whitened per-octave flatness

- **Source pick:** stereo references use the side channel when its 200 Hz–5 kHz energy is ≥ 0.15 × the mid's
  (double-tracked guitars are usually hard-panned; kick, bass and lead vocal are centred and cancel), else mid.
  Overridable (`--source`).
- **Drums out:** median-filter HPSS, Wiener mask, 17-point medians (D. FitzGerald, "Harmonic/percussive separation
  using median filtering", DAFx-10, 2010).
- **Spectrum:** LTAS of active frames (−45 dB gate against the 95th-percentile frame), 34 bands from 50 Hz to 16 kHz,
  normalised to a weighted mean of 0 dB. Fit weights: 1 at 120 Hz–6 kHz, 0.5 at 80–120 Hz and 6–10 kHz, 0 outside
  (bass and cymbal leakage).
- **Gain feature:** spectral flatness (Wiener entropy) per octave 500 Hz–8 kHz, of each frame's power spectrum
  *divided by the LTAS*. A static linear filter scales both by |H|², so the feature is blind to EQ and cab and
  only sees the harmonic fill that clipping adds. Measured on the bundled DI: a 12 dB tilt through the real graphic
  EQ moves it ≤ 0.03 dB per band; a tanh clipper 30 dB into it raises it 3.0 dB on average (`tests/engine/test_tone_profile.cpp`).
- **Rate independence:** the frame (4096 at 44.1/48 kHz), hop and median filters scale with the sample rate, so they
  span the same time and bandwidth at every rate; the tool also resamples the reference to the DI rate before
  profiling (#118 review).

### D3. Search

1. Normalise the DI to −12 dBFS peak (`input.gainDb`, the DI nominal level of tone-targets-a §3.4).
2. Drive candidates: clean + {mid, transparent, dist, fuzz} × drive {0, 2, 4, 6, 8, 10} × tight {off, 100 Hz}
   (49 renders, each profiled).
3. Cab and EQ are linear and after the drive, so their LTAS effect is additive in dB: every cab IR is scored from
   its own band response, and the EQ is a weighted ridge least-squares fit of the 9 graphic gains over a grid of
   low cut × high cut, with Huber IRLS (3 dB) so bands the EQ cannot reach (mix leakage) stop pulling the rest to
   the ±12 dB limits.
4. Score = flatness distance² + 0.5 × spectral residual². The best 3 are refined against the real `Eq` response
   and rendered end to end; the measured best wins. `output.levelDb` puts the render at −18 dBFS RMS.

### D4. Front end, and what is never bundled

`tools/osp-tonematch` reads WAV and MP3 (dr_mp3, the same pinned dr_libs commit as dr_wav). `tools/tonematch/
tonematch.py` (stdlib only) adds YouTube links through **yt-dlp + ffmpeg** and guitar-stem separation through
**Demucs** `htdemucs_6s`, both only if the user installed them. Neither is a dependency, fetched, or shipped, and
downloads/stems stay in the git-ignored `build/tonematch`. The script tells users to download only recordings they
have the right to. Presets it writes are named by the user; factory presets made with it still follow AGENTS.md
rule 2 (no artist or product names).

## Measurements (2026-10-26, MSVC 19.44 Release, bundled CC0 DI and IRs)

| Case | Result | Spectral residual (matched / unmatched) | Flatness distance |
|---|---|---|---|
| Round trip, hidden mid 6 + 412-c + EQ, input −3 dB | mid 10, **412-c** (knob differs: input level normalised) | 0.12 dB / 12.0 dB | 0.15 dB |
| Round trip, hidden clean + 112-a + EQ | clean, **112-a** | 0.14 dB / 1.31 dB | 0.0004 dB |
| Synthetic stereo mix: tanh clipper + 412-e, double-tracked L/R, centred bass + kick | transparent 10, **412-e**, side channel picked | 2.42 dB / 5.21 dB | ≤ 1.3 dB |

The CLI run takes about 40 s for a 10 s DI with 7 IRs (49 drive renders with HPSS); unverified on other CPUs.

## Consequences and limits

- **No amp block yet** (Phase 5): the drive slot is the only gain stage, so high-gain references are approximated
  by drive + cab + EQ. When the amp lands, its channels/models join the candidate list in D3 step 2 and the
  additive cab/EQ stage stays as is.
- A full mix leaks bass, keys and vocals into the profile. The side-channel pick, HPSS, the fit weights and the
  robust fit reduce this, but a Demucs stem (`--stem`) or a solo-guitar section (`--start/--end`) is better.
- Time-based effects (delay, reverb, modulation) and the compressor are not matched in v1.
- The 9-band graphic EQ cannot follow narrow resonances (a pick-up or mic-position notch); that residual is reported.
- The flatness feature compares "harmonic fill", which also depends on playing (chords vs single notes). Playing the
  same part as the reference gives the most reliable gain estimate.
- `input` is a Global-Lock block: with Global Lock on, a preset's matched `input.gainDb` is not applied, and the
  drive then sees the DI at the user's global input level instead of −12 dBFS peak (#119 review).
- The bundled IRs are still "interim / tests-only" (Blocker B-002), and no spec defines `factory:ir/<name>` ids yet;
  the tool uses the file stem until the factory IR set is final (#120 review).
- WAV/MP3 paths over 260 characters fail on Windows (`fopen` in dr_wav/dr_mp3); the tool's UTF-8 manifest fixes
  non-ASCII paths and names, not length.
- - Follow-ups: UI (worker thread, progress, A/B with the reference), the amp candidates above, golden tests for the
  CLI output once the preset loader lands.
