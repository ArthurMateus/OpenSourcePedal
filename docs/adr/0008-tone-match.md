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

### D5. Several guitars and several tones per song (owner request, #117)

`findSections` (`engine/include/osp/engine/ToneSections.h`) splits a guitar recording, ideally the Demucs guitar
stem, into **parts** and per part into **tones**; `osp-tonematch --tones` writes one preset per tone ("guitar 1,
tone 2") with its time ranges and, with `--render`, the matched render plus the song's own section for A/B.
- Parts: left and right become guitar 1 and 2 when one side plays alone in ≥ 20 % of the windows (≥ 12 dB louder),
  or when the median `toneDistance` between the sides, where both play, exceeds 2.5. Otherwise one part from the
  automatic source pick (a double-tracked guitar whose takes share a tone).
- Tones: 4 s windows, 2 s hop; windows 30 dB below the part's loudest are skipped. Average-linkage clustering on
  `toneDistance` = RMS of the residual of 1.25-octave smoothed spectra (the notes matter less than the tone),
  1.5 × the flatness distance and 0.7 × the envelope-spread distance (the gain), stopped at 2.5. Groups shorter than
  6 s join the nearest; then the closest groups merge until at most `maxTones` remain (default 4, `--max-tones`),
  since real arrangements change constantly. A 5-window mode filter over time absorbs runs of ≤ 2 windows (playing
  changes inside a tone). A tone that comes back later is one group with several ranges.
- `Matcher` renders the DI through the drive candidates once; each tone then costs only the cab/EQ fit and three
  refinement renders.

## Measurements (2026-10-26, MSVC 19.44 Release, bundled CC0 DI and IRs)

| Case | Result | Spectral residual (matched / unmatched) | Flatness distance |
|---|---|---|---|
| Round trip, hidden mid 6 + 412-c + EQ, input −3 dB | mid 10, **412-c** (knob differs: input level normalised) | 0.12 dB / 12.0 dB | 0.15 dB |
| Round trip, hidden clean + 112-a + EQ | clean, **112-a** | 0.14 dB / 1.31 dB | 0.0004 dB |
| Synthetic stereo mix: tanh clipper + 412-e, double-tracked L/R, centred bass + kick | transparent 10, **412-e**, side channel picked | 2.42 dB / 5.21 dB | ≤ 1.3 dB |

The CLI run takes about 40 s for a 10 s DI with 7 IRs (49 drive renders with HPSS); unverified on other CPUs.

**Synthetic matrix** (`tools/tonematch/matrix.py`): 54 single-tone runs. The references come from numpy rigs that
share no code with dsp-core: 6 clipper stagings × 3 post-cab EQs × all 7 IRs, on guitar A (bundled strummed DI) and
guitar B (Karplus-Strong single notes, 48 kHz). Each match is scored against the oracle (the DI through the hidden
rig) by an independent LTAS, energy-weighted flatness and envelope spread. Good = tone ≤ 1.5 dB, flatness ≤ 1 dB,
spread ≤ 1.5 dB; fair = 3 / 2 / 3.

| Group | Before envelope spread + weighted flatness | After |
|---|---|---|
| Same guitar, A (18) | 15 good, 3 fair | 18 good |
| Same guitar, B (18) | 0 good, 1 fair, 17 break | 5 good, 9 fair, 4 break |
| Full mix (bass, drums, vocal), A (6) | 3 good, 3 fair | 2 good, 4 fair |
| Cross guitar, A ↔ B (12) | 12 break | 12 break |

Run 1 also showed why: the bundled IRs' long lossy tails raised unweighted flatness by up to 7 dB on sparse playing,
and on single notes clipping lowers flatness (−3 dB) instead of raising it.

**Parts and tones** (synthetic, `--tones`): two guitars in a full mix: 2/2 parts, 2 + 1 tones, boundaries within
1 s; one double-tracked guitar edge → metal → edge: 1 part, 2 tones, within 1 s; two guitars as a clean stem: 2/2
parts, but guitar 1's clean tone split at a strum → arpeggio change (3 tones for 2).

**Real songs** (three commercial recordings supplied by the owner, analysed locally and never committed; the bundled
DI stood in for the player's guitar):

| Song | Full mix | Demucs guitar stem, max 4 tones |
|---|---|---|
| 1: clean guitar + vocals | 7 tones, mostly driven matches (wrong), 3–6 dB | 4 tones, the main tone clean; 2.6–7.5 dB |
| 2: heavy guitars + vocals | 15 tones, 2.5–11.5 dB | 4 tones, all high gain; 2.0–3.0 dB |
| 3: instrumental, effects | 17 tones, mostly driven, 2.7–7.9 dB | 4 tones, mostly clean; 2.6–4.2 dB |

The stem fixes the gain direction (mix leakage reads as extra gain). The rest of the gap to the synthetic results is
the stand-in DI's playing, effects the chain does not model, and stem bleed; unverified by listening.

## Consequences and limits

- **No amp block yet** (Phase 5): the drive slot is the only gain stage, so high-gain references are approximated
  by drive + cab + EQ. When the amp lands, its channels/models join the candidate list in D3 step 2 and the
  additive cab/EQ stage stays as is.
- A full mix leaks bass, keys and vocals into the profile. The side-channel pick, HPSS, the fit weights and the
  robust fit reduce this, but a Demucs stem (`--stem`) or a solo-guitar section (`--start/--end`) is better.
- Time-based effects (delay, reverb, modulation) and the compressor are not matched in v1.
- The 9-band graphic EQ cannot follow narrow resonances (a pick-up or mic-position notch); that residual is reported.
- Both gain features depend on the playing as well as the gain: on the bundled DI a strum → arpeggio change moves
  flatness about as much as clean → crunch. A DI of a different kind of part breaks the match (all 12 cross-guitar
  matrix runs), and inside a song a playing change can read as a tone change. Playing the same part as the
  reference is required for a reliable gain estimate; a DI per section is the next step.
- `input` is a Global-Lock block: with Global Lock on, a preset's matched `input.gainDb` is not applied, and the
  drive then sees the DI at the user's global input level instead of −12 dBFS peak (#119 review).
- The bundled IRs are still "interim / tests-only" (Blocker B-002), and no spec defines `factory:ir/<name>` ids yet;
  the tool uses the file stem until the factory IR set is final (#120 review).
- WAV/MP3 paths over 260 characters fail on Windows (`fopen` in dr_wav/dr_mp3); the tool's UTF-8 manifest fixes
  non-ASCII paths and names, not length.
- Follow-ups: a DI per tone section, UI (worker thread, progress, A/B with the reference), the amp candidates above, golden tests for the
  CLI output once the preset loader lands.
