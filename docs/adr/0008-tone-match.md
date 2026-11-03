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

**Synthetic matrix** (`tools/tonematch/matrix.py`, which computes every figure below): 54 single-tone runs = 36
same-guitar (6 clipper stagings × 3 post-cab EQs × guitars A and B) + 12 cross-guitar + 6 full-mix. Each run uses
one of the 7 bundled IRs, cycled over the runs; the references come from numpy rigs that share no code with dsp-core.
Guitar A is the bundled strummed DI, guitar B Karplus-Strong single notes at 48 kHz. Each match is scored against the
oracle (the DI through the hidden rig) by an independent LTAS, energy-weighted flatness and envelope spread. Good =
tone ≤ 1.5 dB, flatness ≤ 1 dB, spread ≤ 1.5 dB; fair = 3 / 2 / 3. "Before" = the run-1 renders from the binaries
without the #118/#119 envelope-spread fix, re-scored with the same grading; "After" = run 3 at this commit.

| Group | Before envelope spread + weighted flatness | After |
|---|---|---|
| Same guitar, A (18) | 15 good, 3 fair | 18 good |
| Same guitar, B (18) | 0 good, 1 fair, 17 break | 5 good, 9 fair, 4 break |
| Full mix (bass, drums, vocal), A (6) | 3 good, 3 fair | 2 good, 4 fair |
| Cross guitar, A ↔ B (12) | 12 break | 12 break |

Run 1 also showed why: the bundled IRs' long lossy tails raised unweighted flatness by up to 7 dB on sparse playing,
and on single notes clipping lowers flatness (−3 dB) instead of raising it.

**Parts and tones** (synthetic, `--tones`; the CLI prints whole seconds, so 1 s is the resolution): two guitars in a
full mix: 2/2 parts, 2 + 1 tones, worst boundary error 1 s; one double-tracked guitar edge → metal → edge: 1 part,
2 tones, 1 s; two guitars as a clean stem: 2/2 parts, but guitar 1's clean tone split at a strum → arpeggio change
(3 tones for 2). Found and true sections agree spectrally (tone 0.15–1.9 dB on the stem and double-track songs, but
3.5–10 dB in the full mix), while the envelope spread of the driven sections is often 5–7 dB off: on excerpts the gain
estimate is the weakest part.

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

- **Amp search** (A1 below): the classic amps are searched by default. With `--no-amps` (or `searchAmps = false`) the
  drive slot is the only gain stage and high-gain references are approximated by drive + cab + EQ.
- A full mix leaks bass, keys and vocals into the profile. The side-channel pick, HPSS, the fit weights and the
  robust fit reduce this, but a Demucs stem (`--stem`) or a solo-guitar section (`--start/--end`) is better.
- Time-based effects (delay, reverb, modulation) and the compressor are not matched in v1.
- The 9-band graphic EQ cannot follow narrow resonances (a pick-up or mic-position notch); that residual is reported.
- A DI without treble cannot give a bright clean tone: the owner heard the real-song renders as muffled, because the
  bundled lossy DI reads −33.6 dB on `diTrebleDb` (2–6 kHz against 0.5–2 kHz). The tool prints every DI's treble,
  warns below `kDarkDiDb` (−20 dB, a judgement between that DI and a full-band one at −4.3 dB), and flags each
  match whose EQ sits at ±12 dB, so the user knows when the chain ran out of range.
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

## A1. Amp search (2026-11-03, #197)

The classic amps (`ClassicAmpModel`, ADR-0011) are now part of the search. `osp-tonematch` and `runToneMatch` search them by
default; `--no-amps` / `searchAmps = false` keeps the pedal search of D3.

### What the search does

1. **Candidates.** `osp.amp.clean_a`, `crunch_a`, `lead_a` x gain {0, 2, 4, 6, 8, 10}, bass/mid/treble/presence/depth/master
   at 5, rendered through `ClassicAmpModel` at the Live OS-2 quality. The pedal is off: the chain always has an amp
   (parameters.md §2), and pedal-in-front-of-amp combinations are not searched. Candidates are profiled and scored exactly
   as in D3 (flatness + 0.5 spread + 0.5 robust spectral residual of the best cab/EQ over them).
2. **Tone knobs.** For each of the best 3, the effect of bass, mid, treble, presence and depth on the LTAS is measured with
   a +-3 step on real renders (the amp is nonlinear, so the basis belongs to the candidate's gain), and a Huber-robust
   ridge least-squares step moves the knobs towards the reference (at most 5 units, kept in 0...10). A render checks that
   the step improved the score; if not it is dropped. Master stays at 5: it shifts level and saturation, which the gain
   already covers.
3. **Gain trim.** gain -1.5 and +1.5 are rendered and the best score wins (the coarse grid is 2 units).
4. **Cab and EQ.** The refinement of D3 step 4 runs on top, and the best of the 3 end-to-end renders is the result.
5. **Cost.** The candidate and knob renders use the first 15 s of the DI (`searchSeconds`); the final renders use all of it.
   Candidate renders and the 3 refinements run on worker threads.

The preset gets an `amp` block (channel 0, built-in model, the 7 knobs) before the cab. The input is still normalised to
-12 dBFS peak (D3), so the gain knob absorbs the player's level.

### API

`engine/include/osp/engine/ToneMatchApi.h`: `runToneMatch(request, progress)`: reference buffer (mono or stereo) + DI buffer +
conditioned cab IRs in, preset JSON + the match + warnings out. `progress(fraction, stage)` returns false to cancel
(response `cancelled`, no exception). It is called from internal worker threads, one call at a time; run the whole
thing on a worker thread, never the audio or message thread. No JUCE and no I/O in the header: decoding audio
(WAV/MP3, or YouTube through the user's own yt-dlp + ffmpeg) stays in `tools/tonematch`.

### Measurements (2026-11-03, MSVC 19.44 Release, 8 hardware threads, bundled CC0 DI and IRs)

Synthetic round trips (`tests/engine/test_tonematch_amp.cpp`): the DI through a hidden amp + cab, then matched from the same DI.

| Hidden | Matched | Robust residual | Flatness |
|---|---|---|---|
| clean_a gain 2, treble 6.5, 112-a | clean_a gain 0.5, same cab | 0.05 dB | 0.01 dB |
| clean_a gain 4, bass 3, treble 7.5, 412-a | clean_a gain 2.5 (bass 3.5, treble 7.7), same cab | 0.05 dB | 0.05 dB |
| crunch_a gain 5, bass 6, mid 4, treble 6, 412-c | crunch_a gain 4.5 (6.5 / 4.2 / 5.8), same cab | 0.08 dB | 0.13 dB |
| lead_a gain 7, 412-f | lead_a gain 6.5, same cab | 0.13 dB | 0.22 dB |

The test bounds are: model and cab correct, gain within 2 units, robust residual <= 0.5 dB, flatness <= 0.5 dB. Every
run is at least 10x better than the unmatched chain (1.0-7.8 dB).

Runtime: **56.7 s** for a 30 s DI with 4 cabs (the bundled DI looped; 18 amp candidates + refinement; 8 threads). It was
193 s before the 15 s search excerpt and the parallel refinement. The API on a 5 s DI with 2 cabs takes 10.3 s. Not
measured on fewer cores or on CI-class machines: expect roughly the single-thread time divided by the cores you have.

### What this does and does not recover (be honest with users)

- **Recovers well:** the amp model and the cab when the reference is the same signal chain as the candidates, and the
  overall spectrum (residuals above are before any mix leakage). The tone knobs came back close in the two runs that moved
  them (within 0.7 units).
- **Gain is the soft spot.** Within 1.5 units here, with the clean model the weakest (little changes between gain 0.5 and
  2). As in the D3 limits, gain is read from flatness and envelope spread, and these depend on the playing: a strum
  recorded against an arpeggio reads as a different gain, and a different guitar than the song's gives a wrong gain
  (the 12 cross-guitar matrix runs still break; the amp search does not fix that). Use a DI of the same kind of part.
- **Tone knobs and EQ overlap.** A bass/treble shift can be made by a knob, by the graphic EQ, or by the cab choice. The
  knob step is preferred because it changes the signal before the clipping, but two settings that sound alike can have
  different knob values; do not read the knobs as the "true" settings of the recording's amp.
- **Not modelled by the search:** the amp's master, bright and tight, a pedal in front of the amp, and the second and
  third channels (the result is always channel 1). The bundled classic amps are three voicings, not the amp on the
  record: a real recording through another amp is matched by the closest of the three plus the EQ. This is "as close as
  possible", not exact.
- **A real song is harder than a round trip.** The numbers above are synthetic. The synthetic round trips all pass, but
  the real-song results of the table above (2-4 dB residuals) need a Demucs stem and the same part played, and the
  amp search has not yet been compared against real songs by listening (unverified).
- **Odd inputs.** Matching the dry DI against itself, a case with no amp in the reference, gave crunch_a gain 10 with
  extreme knobs (bass 0.8, mid 10, treble 0.3) and the 4 kHz EQ band at -12 dB: when no amp setting explains the spectrum,
  the knob fit can run to its edges. Treat results with knobs at 0 or 10 and an EQ at its limit as "the chain ran out of range"
  (the tool prints the EQ limit warning).
