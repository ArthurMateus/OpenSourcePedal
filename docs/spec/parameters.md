# Parameters

- Issue #23, part 1 of 2: **conventions + pre-amp blocks** (`input`, `gate`, `comp`, `drive1`/`drive2`, `amp`). Part 2
  adds the post-amp blocks (`cab`, `eq`, `mod`, `delay`, `reverb`, `transpose`, `doubler`, `output`) and the global
  settings that are not part of a preset.
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

Smoothing runs in the audio thread with no allocation (one-pole or linear ramp, coefficients recomputed in
`prepare()`). The time is the 0→63 % time constant for one-pole, or the ramp length for linear ramps.

| Class | Default | Method |
|---|---|---|
| Gain / level (`dB`, mix `%`) | 20 ms | Linear ramp of the **linear** gain per sample. No zipper noise (AGENTS.md §4 test) |
| Filter frequency / Q, tone knobs | 20 ms | One-pole on the parameter, TPT/SVF coefficients updated every sample or every 16 samples |
| Nonlinear drive (gain, drive knobs) | 30 ms | One-pole on the pre-gain |
| Time (delay time) | 80 ms | Tape-style glide (modulated read pointer). A crossfade mode is in part 2 |
| Envelope times (attack/release/hold), ratio, threshold | none / 20 ms | Read once per block. Threshold is smoothed at 20 ms |
| `enum`, `bool` inside one block | 10–50 ms crossfade (`xf N`) | Old and new state run in parallel for the crossfade |
| Amp channel, amp model / NAM capture, IR file, preset swap | `xf ADR-0004` | The single owner is ADR-0004 §5 (25 ms), installed at a block boundary once the new state is ready. This spec never repeats the number |

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
  cab and EQ. It is delayed by `L_slot + L_cab` and mixed linearly after the EQ, before post-cab modulation
  (signal-flow §1/§4). 0 % = wet only. 100 % = the delayed amp input only (null ≤ −120 dBFS, REQ-424). The blend value
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

## 3. Open points (part 1)

| Question | Decision / owner |
|---|---|
| Per-model knob curves and the exact `bright`/`tight` responses | amp block ticket (dsp-engineer), Phase 3 |
| Should channel switching from MIDI program change select presets or channels? | part 2 (global MIDI settings) |
| Count of host parameters | 69 after part 1: input 1, gate 7, comp 10, drives 2 × 7, amp 2 + 3 × 10, plus 5 `enabled` (input, gate, comp, drive1, drive2; amp is always on). The final count comes in part 2 (v1 total 226) |
