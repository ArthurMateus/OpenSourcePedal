# Parameters

- Issue #23. §1 conventions, §2 pre-amp blocks (`input`, `gate`, `comp`, `drive1`/`drive2`, `amp`), §3 post-amp blocks
  (`cab`, `eq`, `mod`, `delay`, `reverb`, `transpose`, `doubler`, `output`), §4 global settings that are not part of a
  preset, §5 totals, requirements and open points.
- Author: architect (acct1/Lyra), 2026-10-25. Rests on `docs/research/reference-plugins.md` (§2.3 measured ranges,
  Rec. 2–7), `tone-targets-a.md` §3.1/§3.4, `tone-targets-b.md` §2.3/§3.1, `nam-runtime.md` (Calibration rec.),
  `oversampling.md` §1, ADR-0003 (preset schema, PR #49).

## 1. Conventions

### 1.1 Ids

- **Full id** = `<blockId>.<key>`, e.g. `comp.attackMs`. Amp-channel parameters are `amp.ch<N>.<key>` (N = 1…3), and
  per-channel blocks follow the same pattern (`eq.ch2.b1kDb`). In a preset (ADR-0003) the block id and channel are
  implied by the position, so the JSON key is just `<key>`.
- Keys are lowerCamelCase with a **unit suffix** where there is a unit: `Db`, `Ms`, `Hz`, `Pct`, `S`, `St`. Knob-style
  values have no suffix.
- **Ids are permanent.** They are preset keys and the host automation identity. JUCE derives the VST3 `ParamID` from the
  string id, so renaming one breaks saved host projects. A rename needs a preset migration (ADR-0003 §5) **and** a
  deprecated alias kept forever.
- The `enabled` flag of every block is a parameter too: `<blockId>.enabled` (bool, host-visible, automatable, smoothed
  by a **constant-amplitude (linear)** crossfade between the processed and dry signal, because the two are highly
  correlated and an equal-power fade would bump the level by up to +3 dB. The fade length is owned by
  `docs/spec/signal-flow.md` §5 (#52).

### 1.2 Units and value types

| Unit | Meaning | Stored as |
|---|---|---|
| `dB` | level or gain | float, linear in dB |
| `ms`, `s` | time | float |
| `Hz` | frequency | float |
| `%` | proportion 0–100 | float |
| `knob` | panel-style 0–10 control, the scale the research targets use (tone-targets-a T4, tone-targets-b §2.2 rig table) | float |
| `ratio` | compression ratio, n:1 | float |
| `enum` | named choice | string (listed values only) |
| `bool` | switch | boolean |

### 1.3 Skew (how the 0–1 host value maps to the range)

| Skew | Mapping v(x), x ∈ [0, 1] | Used for |
|---|---|---|
| `lin` | min + x·(max − min) | dB, %, knob, pan |
| `log` | min·(max/min)^x | Hz, ms/s times, ratio |
| `step` | discrete, equally spaced over the values | enum, bool, channel select |

A `log` skew needs min > 0. Where a range starts at 0 (e.g. hold 0–200 ms), the spec gives `lin`.

### 1.4 Smoothing

Smoothing runs in the audio thread with no allocation (one-pole or linear ramp). Smoother time-constant
coefficients are computed in `prepare()`. The time is the 0→63 % time constant for one-pole, or the ramp length
for linear ramps. A processing sample is a sample at the block's actual processing rate, including oversampling.

| Class | Default | Method |
|---|---|---|
| Gain / level (`dB`, mix `%`) | 20 ms | Linear ramp of the **linear** gain per sample. No zipper noise (AGENTS.md §4 test) |
| Filter frequency / Q, tone knobs | 20 ms | One-pole on the parameter or the stage-specified filter coefficient; applied coefficients evolve **every processing sample** while moving. Target design and per-sample application are separate (see below) |
| Nonlinear drive (gain, drive knobs) | 30 ms | One-pole on the pre-gain |
| Time (delay time) | 80 ms | Tape-style glide (modulated read pointer). A crossfade mode is in part 2 |
| Envelope times (attack/release/hold), ratio, threshold | none / 20 ms | Read once per block. Threshold is smoothed at 20 ms |
| `enum`, `bool` inside one block | 10–50 ms crossfade (`xf N`) | Old and new state run in parallel for the crossfade |
| Amp channel, amp model / NAM capture, IR file, preset swap | `xf ADR-0004` | The single owner is ADR-0004 §5 (25 ms), installed at a block boundary once the new state is ready. This spec never repeats the number |

**Filter/tone coefficient timing (clarified 2026-10-28).** Parameter targets are delivered at block boundaries;
the applied coefficient trajectory must still advance per processing sample. Expensive target/design work may run
only on a changed value or at a bounded sub-block cadence, provided stable per-sample coefficient glides/ramps track
the stage's specified smoothing law. Holding coefficients constant between updates is not equivalent: the
16-sample hold in [PR #87](https://github.com/ArthurMateus/OpenSourcePedal/pull/87) measured −55 dBc on a 0↔10
tone step and failed REQ-TEST-003 (source accessed 2026-10-28).

Every implementation must pass the unchanged [REQ-TEST-003](../adr/0007-test-strategy.md) gate: RMS residual
≤ −60 dBc against an offline double-precision reference applying the specified law per sample from the delivery
sample, normalised to the larger settled steady-state RMS. A sparse design cadence does not relax that gate.
The merged Phase 5 stages demonstrate two permitted implementations (sources accessed 2026-10-28):

- `PreampStage` computes target TPT coefficients only when a corner value changes, then applies a 20 ms one-pole
  glide to those coefficients every sample. Its reported drive/corner step residual is −93.6 dBc
  ([PR #183](https://github.com/ArthurMateus/OpenSourcePedal/pull/183)).
- `ToneStack` applies 20 ms one-poles to the knob values and designs at adaptive chunks nominally 1/1500 s,
  rounded to whole samples (≤0.67 ms at supported processing rates, shortened at block boundaries and for large
  log-resistance changes). It linearly ramps its modal coefficients every sample across each chunk. Its reported
  worst knob-step residual is −90.8 dBc at 192 kHz
  against a double-precision reference designed every sample
  ([PR #185](https://github.com/ArthurMateus/OpenSourcePedal/pull/185)).

### 1.5 Automatable and visibility

- **Auto** = exposed as a host-automatable parameter, MIDI-learnable, and recorded in the plugin state.
- **Vis** `H` = host-visible (VST3 parameter list). `P` = preset-only: stored in the preset and editable in the UI, but
  not exposed to the host, because changing it needs a non-RT reload (model or IR choice) or because it is a structural
  option. Preset-only parameters are changed from the message thread and handed over through the snapshot mechanism
  (threading spec, #25). `reserved` = a post-v1 feature (requirements.md priority P). The id is fixed now so it can
  never be reused. It is not in the v1 host list, and a v1 build preserves it in presets but ignores it (ADR-0003 §3).
- The host parameter list is **fixed at plugin load** (VST3 requires a static list). All 3 amp channels are always
  exposed.

### 1.6 Common rules

- Ranges are inclusive. Out-of-range preset values are clamped (ADR-0003 §6).
- Defaults give a neutral, usable sound with the block enabled. Every block is **disabled by default** except `input`,
  `amp`, `cab` and `output`. `amp` has no `enabled` parameter, and its preset `enabled` is always `true`
  (the chain always has an amp stage; a clean console model covers "no amp").
- The oversampling **quality** (Live / HQ, ADR-0005 D6) is a global setting (part 2),
  not a preset parameter, because it trades CPU for aliasing on the user's machine. The sound difference is limited to
  aliasing.

## 2. Pre-amp blocks

Columns: **Range** (min…max, or values), **Def** default, **Sk** skew, **Sm** smoothing (ms, `xf` = crossfade), **Auto**,
**Vis**.

### 2.1 `input`: input gain (Global-Lock block)

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `input.gainDb` | Input | −24…+24 | dB | 0 | lin | 20 | ✔ | H |

The reference has an input knob next to the gate (reference-plugins §2.4). The range lets a −12 dBFS-peak DI
(tone-targets-a §3.4) be trimmed by ±24 dB to hit the amp models' sweet spot.

### 2.2 `gate`: noise gate (Global-Lock block)

From tone-targets-b §3.1 (gate row) and §3.3 (calibration).

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `gate.thresholdDb` | Threshold | −90…−20 | dB | −55 | lin | 20 | ✔ | H |
| `gate.hysteresisDb` | Hysteresis | 0…10 | dB | 4 | lin | — | ✔ | H |
| `gate.attackMs` | Attack | 0.1…10 | ms | 0.5 | log | — | ✔ | H |
| `gate.holdMs` | Hold | 0…200 | ms | 20 | lin | — | ✔ | H |
| `gate.releaseMs` | Release | 10…500 | ms | 80 | log | — | ✔ | H |
| `gate.rangeDb` | Range | −90…0 | dB | −90 | lin | 20 | ✔ | H |
| `gate.detector` | Detector | `peak`, `rms` | enum | `peak` | step | — | — | P |
| `gate.postAmp` | Post-amp tap | bool | bool | false | step | xf 10 | ✔ | H |

`gate.postAmp` = gain applied after the amp, keyed from the input (tone-targets-b open Q1, answered: one gate with an
optional post tap). The −55 dB default assumes the −12 dBFS DI reference (tone-targets-b §3.3).

### 2.3 `comp`: compressor (first tone block)

From tone-targets-a §3.1 (compressor row) and T2/T3, plus reference-plugins §2.2 (dual comp, slow/fast).

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `comp.thresholdDb` | Threshold | −50…0 | dB | −20 | lin | 20 | ✔ | H |
| `comp.ratio` | Ratio | 1…20 | ratio | 4 | log | — | ✔ | H |
| `comp.kneeDb` | Knee | 0…12 | dB | 6 | lin | — | ✔ | H |
| `comp.attackMs` | Attack | 0.1…100 | ms | 10 | log | — | ✔ | H |
| `comp.releaseMs` | Release | 20…1000 | ms | 150 | log | — | ✔ | H |
| `comp.blendPct` | Blend | 0…100 | % | 100 | lin | 20 | ✔ | H |
| `comp.makeupDb` | Makeup | 0…24 | dB | 0 | lin | 20 | ✔ | H |
| `comp.autoMakeup` | Auto makeup | bool | bool | true | step | xf 10 | ✔ | H |
| `comp.mode` | Mode | `opto`, `vca`, `dual` | enum | `opto` | step | xf 10 | ✔ | H |
| `comp.tone` | Tone | 0…10 | knob | 5 | lin | 20 | ✔ | H |

- `opto` = program-dependent two-stage release. `dual` = two stages in series, the second with 4× the attack.
- `tone` is a ±6 dB tilt around 1 kHz at the comp output (the reference opto comp has a tilt EQ, tone-targets-a §2.1).
- Funk-clean target: GR 6–10 dB, attack 10–30 ms, blend 60–80 % (tone-targets-a T2).

### 2.4 `drive1`, `drive2`: overdrive / boost (two identical slots)

From tone-targets-b F1–F3 and §3.1 (TS-style boost, "attack" HPF), and reference-plugins Rec. 1 (2 drive slots).

| Id (`driveN.`…) | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `type` | Type | `mid` (TS-style), `transparent`, `dist`, `fuzz` | enum | `mid` | step | xf 10 | ✔ | H |
| `drive` | Drive | 0…10 | knob | 2 | lin | 30 | ✔ | H |
| `tone` | Tone | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `levelDb` | Level | −20…+12 | dB | 0 | lin | 20 | ✔ | H |
| `lowCutHz` | Tight | 20…300 | Hz | 20 | log | 20 | ✔ | H |
| `bright` | Bright | bool | bool | false | step | xf 10 | ✔ | H |
| `mixPct` | Mix | 0…100 | % | 100 | lin | 20 | ✔ | H |

- `lowCutHz` is the 2nd-order pre-clip HPF. The UI offers detents at 40/60/80/100/130/170 Hz (tone-targets-b F3), and
  20 Hz = off.
- The tight-boost recipe is `type=mid, drive=0, levelDb=+8…+10, tone=5` (tone-targets-b F2 and §3.3 table).
- Oversampling: `drive1` and `drive2` share one oversampling region (OS-1) at the hungriest voicing's factor, so a
  `type` change or bypass never changes the factor or the latency. See ADR-0005 D2 (#50). The ADAA order per voicing
  is set there too.

### 2.5 `amp`: amplifier (3 channels)

From reference-plugins Summary 3 and Rec. 2, tone-targets-b F6 (depth/presence) and §3.1, tone-targets-a T1 (DI blend),
nam-runtime (Calibration rec.).

Block-level parameters:

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `amp.channel` | Channel | 1, 2, 3 | enum | 1 | step | xf ADR-0004 | ✔ | H |
| `amp.cleanBlendPct` | Clean blend | 0…100 | % | 0 | lin | 20 | ✔ | H |
| `amp.cabLink` | Link cab to channel | bool | bool | true | step | — | — | P |

- v1 has exactly **3 channels**. The schema allows up to 4 (ADR-0003) for later. A 4th channel would need new host
  parameters, which means a new plugin version.
- In the preset, `amp.channel` is stored as `activeChannel` (0-based, ADR-0003). The host value is 1-based for
  readability.
- `cleanBlendPct` is the v1 clean DI blend (REQ-424; tone-targets-a T1, open Q3 answered: a path on the amp block).
  It is a parallel path tapped at the amp input (after `drive2` and the pre-amp modulation position) that skips amp,
  cab and EQ. It is delayed by `K_clean` (signal-flow §4, ADR-0009 D4) and mixed linearly after the EQ, before
  post-cab modulation (signal-flow §1/§4). 0 % = wet only. 100 % = the delayed amp input only (null ≤ −120 dBFS, REQ-424). The blend value
  never changes `L_total`.

Per-channel parameters (`amp.ch<N>.` …, N = 1…3; each channel is a built-in model or a NAM capture, ADR-0003):

| Key | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `model` | Model | built-in id or NAM capture | — | ch1 `osp.amp.clean_a`, ch2 `osp.amp.crunch_a`, ch3 `osp.amp.lead_a` | — | xf ADR-0004 | — | P |
| `gain` | Gain | 0…10 | knob | ch1 3, ch2 5, ch3 6 | lin | 30 | ✔ | H |
| `bass` | Bass | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `mid` | Middle | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `treble` | Treble | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `presence` | Presence | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `depth` | Depth | 0…10 | knob | 5 | lin | 20 | ✔ | H |
| `master` | Master | 0…10 | knob | 5 | lin | 30 | ✔ | H |
| `levelDb` | Level | −24…+12 | dB | 0 | lin | 20 | ✔ | H |
| `bright` | Bright | bool | bool | false | step | xf 10 | ✔ | H |
| `tight` | Tight | bool | bool | false | step | xf 10 | ✔ | H |
| `namLevelMode` | Capture level | `raw`, `normalized`, `calibrated` | enum | `normalized` | step | xf ADR-0004 | — | P |

- `levelDb` is the tone-neutral channel output, the reference's LEVEL/OUTPUT ("volume without affecting tone",
  reference-plugins §2.3). `master` drives the power-amp stage and does change the tone.
- `presence` is a 2–6 kHz high shelf, ±10 dB at 0/10 (tone-targets-b F6). `depth` is a 60–150 Hz low shelf, ±10 dB.
  For white-box models both sit inside the power-amp feedback loop. For NAM channels they are static shelves after the
  capture.
- **NAM channels:** `gain` maps to an input trim (0…10 → −20…+20 dB, 5 = 0 dB). `bass`/`mid`/`treble` become a static
  3-band EQ after the capture (100 Hz shelf / 800 Hz peak / 3 kHz shelf, ±12 dB). `master` and `tight` are ignored and
  greyed out. `namLevelMode` follows nam-runtime: `normalized` applies `−18 − loudness`, and it falls back to `raw` when
  the capture has no `loudness` metadata. The interface calibration level `inputInterfaceLevel_dBu` is a global setting
  (part 2, ADR-0003 §7).
- `bright` / `tight` are the voicing switches found across the reference amps (reference-plugins §2.3: Bright, Bite,
  Tight). Their exact curves are per model and are set in the amp block ticket.
- The tone-stack topology per built-in model (and what the knob positions mean in dB) is defined in the amp block
  ticket. Only ids, ranges and defaults are fixed here.

## 3. Post-amp blocks

### 3.1 `cab`: two IR slots

From reference-plugins Summary 4 and Rec. 3, and ADR-0003 §4/§5 (resources by uri + sha256). With `amp.cabLink` on, the
cab state follows the amp channel (`perChannel`, ADR-0003), so the host ids are `cab.ch<N>.<key>`. With it off, ch1's
values are used for every channel.

| Key (`cab.ch<N>.`…) | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `ir1`, `ir2` (resources) | IR file | factory or user WAV | — | ir1 per amp model, ir2 none | — | xf ADR-0004 | — | P |
| `irK.enabled` (K = 1, 2) | Slot on | bool | bool | ir1 true, ir2 false | step | xf 10 | ✔ | H |
| `irK.levelDb` | Level | −60…+6 | dB | 0 | lin | 20 | ✔ | H |
| `irK.pan` | Pan | −100…+100 | % | 0 | lin | 20 | ✔ | H |
| `irK.invert` | Phase invert | bool | bool | false | step | xf 10 | ✔ | H |
| `irK.delayMs` | Align | 0…2 | ms | 0 | lin | 20 | ✔ | H |

- `irK.delayMs` time-aligns two IRs for the mic blend (comb-filter control). The reference has only phase invert, so
  this is our addition.
- Mic **position/distance** is not a parameter in v1. Choosing a position means choosing a different IR file from a
  multi-position set (reference-plugins Rec. 3). Room send is deferred.
- An IR change is preset-only, because loading and partitioning the IR happens on the loader thread.
- Every IR (factory or user) is normalised at load to 0 dB gain for pink noise over 40 Hz–20 kHz
  ([ADR-0010](../adr/0010-cab-ir-level.md)), so enabling the cab or changing the IR keeps the level; `irK.levelDb`
  trims from there.

### 3.2 `eq`: one block, three voicings

Answers tone-targets-b open Q5: **one block** with a `mode`. The bands come from reference-plugins Rec. 4 (9-band octave
graphic plus HPF/LPF), tone-targets-b F5/§3.1 (5-band 80/240/750/2200/6600 Hz voicing) and §3.1 (4-band
semi-parametric). Only the bands of the active mode process audio, but every mode's values are stored. The state is
`perChannel` and follows the amp channel when `eq.followChannel` is on (the reference binds one EQ to each amp,
reference-plugins §2.4).

| Key (`eq.ch<N>.`…) | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `mode` | Mode | `graphic9`, `graphic5`, `parametric` | enum | `graphic9` | step | xf 20 | — | P |
| `g63Db` … `g16kDb` (63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k) | Graphic band | −12…+12 | dB | 0 | lin | 20 | ✔ | H |
| `m80Db`, `m240Db`, `m750Db`, `m2200Db`, `m6600Db` | 5-band | −12…+12 | dB | 0 | lin | 20 | ✔ | H |
| `pK.freqHz` (K = 1…4) | Band freq | 20…20000 | Hz | 100 / 500 / 2000 / 6000 | log | 20 | ✔ | H |
| `pK.gainDb` | Band gain | −15…+15 | dB | 0 | lin | 20 | ✔ | H |
| `pK.q` | Band Q | 0.3…6 | ratio | 0.7 | log | 20 | ✔ | H |
| `p1.shape`, `p4.shape` | Edge band shape | `peak`, `shelf` | enum | `shelf` | step | xf 20 | — | P |
| `lowCutHz` | Low cut | 20…500 | Hz | 20 | log | 20 | ✔ | H |
| `highCutHz` | High cut | 2000…20000 | Hz | 20000 | log | 20 | ✔ | H |
| `levelDb` | Level | −12…+12 | dB | 0 | lin | 20 | ✔ | H |

- Block level: `eq.followChannel` (bool, default true, P).
- Graphic Q is constant per voicing, not a parameter: about 1.0 for the octave bands and 0.7 for the 5-band voicing
  (tone-targets-b §3.1 gives Q ≈ 0.7–1.0, *unverified*).
- `lowCutHz` and `highCutHz` are 2nd order (12 dB/oct).

### 3.3 `mod`: modulation

From reference-plugins §2.3 (rate 0.10–10 Hz) and Rec. 5, and tone-targets-a §3.1 (chorus modes).

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `mod.type` | Type | `chorus80s`, `chorusWide`, `flanger`, `phaser`, `vibe`, `tremolo` | enum | `chorusWide` | step | xf 30 | ✔ | H |
| `mod.rateHz` | Rate | 0.1…10 | Hz | 0.8 | log | 20 | ✔ | H |
| `mod.sync` | Sync | `off`, `1/1`, `1/2`, `1/4`, `1/8`, `1/16` (+ `D`/`T` variants) | enum | `off` | step | — | ✔ | H |
| `mod.depthPct` | Depth | 0…100 | % | 50 | lin | 20 | ✔ | H |
| `mod.mixPct` | Mix | 0…100 | % | 50 | lin | 20 | ✔ | H |
| `mod.feedbackPct` | Feedback | −95…+95 | % | 0 | lin | 20 | ✔ | H |
| `mod.widthPct` | Stereo width | 0…100 | % | 100 | lin | 20 | ✔ | H |
| `mod.position` | Position | `post`, `pre` | enum | `post` | step | — | — | P |

`mod.position` = `pre` places the block **after `drive2`, before the amp**. That is the classic pedalboard order, and 3
reference products do it (reference-plugins Summary 6). This is the orchestrator's decision on #51, and
`docs/spec/signal-flow.md` (#52) is being updated to match. `post` (default) places it after the cab/EQ (REQ-415).
The schema already allows both (ADR-0003 §4).

### 3.4 `delay`

From reference-plugins §2.3 (7–3000 ms, sync 1/64T…1/1D, low cut 50–800 Hz, high cut 0.5–5 kHz) and Rec. 5, plus
tone-targets-a §3.1 (modes, ducking).

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `delay.mode` | Mode | `digital`, `analog`, `tape` | enum | `digital` | step | xf 30 | ✔ | H |
| `delay.timeMs` | Time | 7…3000 | ms | 400 | log | 80 glide | ✔ | H |
| `delay.sync` | Sync | `off`, `1/64T` … `1/1D` (straight, dotted, triplet) | enum | `off` | step | 80 glide | ✔ | H |
| `delay.feedbackPct` | Feedback | 0…100 | % | 30 | lin | 20 | ✔ | H |
| `delay.mixPct` | Mix | 0…100 | % | 25 | lin | 20 | ✔ | H |
| `delay.lowCutHz` | Low cut | 50…800 | Hz | 80 | log | 20 | ✔ | H |
| `delay.highCutHz` | High cut | 500…16000 | Hz | 5000 | log | 20 | ✔ | H |
| `delay.stereo` | Stereo | `mono`, `pingpong`, `wide` | enum | `mono` | step | xf 30 | ✔ | H |
| `delay.modDepthPct` | Modulation | 0…100 | % | 0 | lin | 20 | ✔ | H |
| `delay.saturationPct` | Saturation | 0…100 | % | 20 | lin | 20 | ✔ | H |
| `delay.duckDb` | Ducking | 0…24 | dB | 0 | lin | 20 | ✔ | H |

- Feedback is limited internally to a loop gain ≤ 0.995 at 100 %, so the delay rings but never runs away.
- `highCutHz` extends the reference's 5 kHz top to 16 kHz for the digital mode. The 5 kHz default matches the reference.
- Tap tempo is a UI action that writes `delay.timeMs`, or the tempo when synced. It is not a parameter.

### 3.5 `reverb`

From reference-plugins §2.3 (HPF 50–700 Hz, LPF 1–10 kHz, decay 1–8.5 s) and Rec. 5, and tone-targets-a §3.1 (freeze,
shimmer +12 st).

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `reverb.mode` | Mode | `room`, `hall`, `plate`, `spring` (S), `shimmer` | enum | `hall` | step | xf 50 | ✔ | H |
| `reverb.decayS` | Decay | 0.3…20 | s | 2.5 | log | 20 | ✔ | H |
| `reverb.preDelayMs` | Pre-delay | 0…200 | ms | 20 | lin | 80 glide | ✔ | H |
| `reverb.mixPct` | Mix | 0…100 | % | 20 | lin | 20 | ✔ | H |
| `reverb.lowCutHz` | Low cut | 50…700 | Hz | 100 | log | 20 | ✔ | H |
| `reverb.highCutHz` | High cut | 1000…10000 | Hz | 6000 | log | 20 | ✔ | H |
| `reverb.modPct` | Modulation | 0…100 | % | 20 | lin | 20 | ✔ | H |
| `reverb.shimmerPct` | Shimmer | 0…100 | % | 0 | lin | 20 | ✔ | H |
| `reverb.shimmerInterval` | Shimmer pitch | `octUp`, `fifthUp`, `octDown` | enum | `octUp` | step | xf 50 | ✔ | H |
| `reverb.freeze` | Freeze | bool | bool | false | step | xf 50 | ✔ | H |

The decay range extends the reference's 1–8.5 s to 0.3–20 s for the ambient targets (tone-targets-a A1/A6). `freeze`
holds the tank (feedback = 1, input muted). `spring` goes beyond REQ-417 (room/hall/plate/shimmer, priority M) and is
priority **S**. If it slips, the enum value stays reserved and falls back to `plate`.

### 3.6 `transpose` (reserved, post-v1), `doubler`, `output` (Global-Lock blocks)

| Id | Name | Range | Unit | Def | Sk | Sm | Auto | Vis |
|---|---|---|---|---|---|---|---|---|
| `transpose.semitones` | Transpose (post-v1) | −12…+12, integer | st | 0 | step | xf 30 | — | reserved |
| `doubler.amountPct` | Amount | 0…100 | % | 50 | lin | 20 | ✔ | H |
| `doubler.widthPct` | Width | 0…100 | % | 100 | lin | 20 | ✔ | H |
| `doubler.delayMs` | Offset | 10…30 | ms | 18 | lin | 80 glide | ✔ | H |
| `doubler.detuneCents` | Detune | 0…25 | cents | 8 | lin | 20 | ✔ | H |
| `output.levelDb` | Output | −60…+12 | dB | 0 | lin | 20 | ✔ | H |
| `output.limiter` | Safety limiter | bool | bool | true | step | xf 10 | ✔ | H |

- Transpose ±12 st is in every reference product (reference-plugins Summary 7), but REQ-420 makes it **P (post-v1)**.
  `transpose.semitones` and `transpose.enabled` are reserved and are not in the v1 host list.
- The doubler ranges follow tone-targets-b §3.1 (10–30 ms, about ±10 cents, *unverified*).
- `output.limiter` is a −0.3 dBFS true-peak safety limiter. Golden renders are made at −18 LUFS-I (tone-targets-a §3.4),
  so they normally never reach it.
- `output` has no `enabled` parameter.

## 4. Global settings (not in presets)

Stored in `settings.json` (ADR-0003 §7). These are not host parameters.

| Key | Range / values | Default | Source |
|---|---|---|---|
| `inputMode` | `mono`, `stereo` | `mono` | reference-plugins §2.4 |
| `globalLock` | bool (skips `input`, `gate`, `transpose`, `doubler`, `output` on preset load) | false | reference-plugins §2.4 |
| `oversamplingQuality` | `live`, `hq` | `live` (offline render: `hq`) | ADR-0005 D6 (#50) |
| `namInterfaceLevelDbu` | −60…+60 dBu | 12 | nam-runtime Calibration rec. |
| `tuner.referenceHz` | 415…466 Hz | 440 | our choice |
| `tuner.muteOutput` | bool | true | our choice |
| `tempo.source` | `host`, `global` (the standalone always uses `global`) | `host` | reference-plugins §2.4 |
| `tempo.bpm` | 20…300 | 120 | ADR-0003 `tempoBpm` fallback |
| `metronome.*` | on/off, level, accents (standalone only) | off | reference-plugins §2.4 |
| `midi.programChange` | `presets`, `channels`, `off` | `presets` | answers the part-1 open point |
| `midi.mappings` | list of {source CC/note/PC, param id, absolute/relative, min, max} | empty | reference-plugins §2.4 |
| `ui.scale` | 50…200 % | 100 | reference-plugins §2.4 (resizable) |

`midi.programChange = channels` maps PC 0–2 to `amp.channel` 1–3, so a channel switch needs no reload (ADR-0003 §3).

## 5. Totals, requirements and open points

**Host-visible parameters (v1): 226.**
- Part 1: 69.
- Part 2: 157, made up of cab 3 × 10, eq 3 × 29, mod 7, delay 11, reverb 10, doubler 4, output 2, plus 6 `enabled`
  (cab, eq, mod, delay, reverb, doubler).
- Reserved for post-v1: `transpose.semitones` and `transpose.enabled`.

That is well within what VST3 hosts handle. REQ-PAR-001 makes CI fail if the list changes without a spec update.

| ID | Requirement | Test |
|---|---|---|
| REQ-PAR-001 | The host parameter list (ids, ranges, defaults) equals this spec. Ids never disappear | `osp_tests` compares the engine parameter table with a generated `tests/spec/parameters.json` |
| REQ-PAR-002 | Every parameter can sweep min→max in 10 ms with no NaN/Inf and no zipper noise above −90 dBFS on a 1 kHz sine | Smoothing test per class (§1.4), ADR-0007 parameter fuzz |
| REQ-PAR-003 | The normalized ↔ value round-trip is exact to 1e-6 relative for every skew | Unit test |

| Question | Decision / owner |
|---|---|
| Per-model knob curves and the exact `bright`/`tight` responses | amp block ticket (dsp-engineer), Phase 3 |
| Allowed orders (mod pre/post, drive slots) | `docs/spec/signal-flow.md` (#22) |
| Transpose and doubler latency: report it only while enabled (the latency then changes at runtime), or always? | threading / signal-flow (#25, #22), together with the pitch-shifter choice (#11) |
| Mic position as a continuous parameter (needs a multi-position IR set) | post-v1, after the IR asset ticket (#7) |
