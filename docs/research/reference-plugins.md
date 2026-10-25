# Reference lineup: artist-signature amp-sim plugins, signal chains and controls

- **Issue:** #3
- **Author:** acct1/researcher (Lyra)
- **Date:** 2026-10-25
- **Method:** public product pages, public PDF user manuals (text layer only), the vendor's news and downloads pages, and
  trade press. No binaries were downloaded or opened. The PDF manuals render knob labels as artwork, so many
  per-knob names are **not** in the text layer. Those are marked *unverified* below.
- **Internal note.** Third-party names appear here only. They must never reach product paths (AGENTS.md rule 2). All
  names found here were added to `tools/guard/forbidden_names.txt`.

## 1. Summary (for the architect)

1. The reference line is **Neural DSP's "Archetype" series: 12 artist products** (§2.1), plus about 10 non-artist amp
   suites. Since 2023 the vendor has been replacing the originals with "X" editions that share one framework and UI
   ([S2], [S3]).
2. **Every product shares one skeleton:** `Input + Gate → [special/pitch FX] → Pre FX (3–5 pedals in series) → Amp
   (3–4 channels, 1 active) → Cab (mic'd IRs + IR loader) → EQ (one per amp) → Post FX (2–3 time-based pedals) →
   Transpose → Doubler → Output` ([S5]–[S18]). That skeleton is our minimum "complete" product.
3. **Amps:** **3 channels is the norm** (clean / crunch-or-rhythm / lead). Two products have 4 channels and one has 1
   amp with 2 channels. Each amp has its own EQ, and the amp selector switches the EQ too ([S5], [S6], [S8]).
4. **Cab:** 6–11 virtual mics. A mic is placed by dragging it or with POSITION and DISTANCE knobs. Mono or stereo
   (a second mic with LEVEL, PAN and PHASE). A ROOM send per mic. A **custom IR loader** per mic slot (LEVEL, PAN and
   PHASE stay active, POSITION and DISTANCE are disabled). Amp↔cab **Link/Unlink** ([S5]–[S8], [S10], [S14]). This
   matches our "two IRs with mic blend" plan. Add per-slot phase invert and pan.
5. **EQ:** a **9-band graphic EQ with HPF/LPF** in 9 of 12 products, a 4-band semi-parametric EQ in 3. Use graphic
   by default. The parametric EQ that MASTER_PROMPT asks for covers the semi-parametric case.
6. **Post FX are always chorus/delay/reverb.** Shimmer reverb appears in 4 products, and tape or BBD delay flavours in
   most of them. Modulation (phaser, chorus, flanger, vibe) sits **pre-amp** in 3 products. Our modulation block
   should be placeable pre or post (see the open questions).
7. **Global features in every X-generation product:** chromatic tuner (plus an always-on "live tuner" strip),
   **Transpose ±12 st**, **Doubler** (simulated stereo double-tracking), metronome and tap tempo (standalone only),
   MIDI learn (CC/PC/Note, absolute or relative), resizable window, and section bypass by right-click or double-click
   ([S5], [S8]). Newest products (2025–26) add **Global Lock**, a **Preset Explorer** (search, tags, favourites) and
   **Undo/Redo** ([S16], [S17], [S19]).
8. **Preset rules:** input mode, tuner, metronome and the MIDI map are **not** stored in presets ([S5]). Unsaved
   changes show an asterisk. User folders and subfolders are supported. Custom IR paths are stored in the preset.
9. **Signature extras** (pitch shifter "whammy", polyphonic octaver, synth, multi-voice harmonizer, granular glitch,
   ring mod, envelope filter, wah) show up as **one or two special blocks per product**. They are not part of the
   common core, so they belong in post-v1 tickets.

## 2. Findings

### 2.1 Lineup (artist "Archetype" series)

Years are the first release unless marked. Shown as "X: yyyy" when an X edition exists.

| Product (internal name) | First release | X edition | Amps (channels) | Source |
|---|---|---|---|---|
| Archetype: Plini | 2018 ("five years ago" in a 2023 manual) | X: 2023 | 3: Clean / Crunch / Lead | [S5], [S9] |
| Archetype: Abasi | *unverified* (≈2019) | none (v2.0.1, 2023) | 3: Clean / Rhythm / Lead | [S2], [S12] |
| Archetype: Nolly | *unverified* (≈2020) | X: 2024 | 4: Clean / Crunch / Rhythm / Lead | [S8], [S11] |
| Archetype: Cory Wong | 2020-07-06 | X: 2024 | 3: DI console / Clean / Edge-of-breakup | [S7], [S10], [S21] |
| Archetype: Gojira | 2021 | X: 2023 | 3: Clean / Rust / Hot | [S6], [S9b] |
| Archetype: Tim Henson | 2021-07-30 | X: *unverified* | 3: acoustic-sim / British low-gain (2 ch) / Lead | [S13], [S22] |
| Archetype: Petrucci | 2021-12 | X: ≤2024-11 | 4: Piezo / Clean / Rhythm / Lead | [S2], [S14], [S23] |
| Archetype: Rabea | *unverified* (≈2022) | X: *unverified* | 3: Clean / Rhythm / Lead | [S15] |
| Archetype: Tom Morello | ≤2023-05 (v1.0.0) | none, desktop only | 1 amp, 2 channels | [S2], [S18] |
| Archetype: Mateus Asato | *unverified* (≈2023) | none (v1.1.0, 2024) | 3: Vintage Clean / Modern Clean / Lead | [S2], [S17b] |
| Archetype: Misha Mansoor X | 2025-09-17 | born X | 3: Clean / Rhythm / Lead | [S16], [S20] |
| Archetype: John Mayer X | *unverified* (≤2026-04) | born X | 3 in parallel ("three-in-one") | [S17], [S19] |

Non-artist suites from the same vendor (context only, not the reference): Fortin Nameless / Cali / NTS, Soldano
SLO-100, Mesa Mark IIC+, Morgan, Omega Granophyre, Tone King Imperial MKII, Parallax and Darkglass Ultimate (bass),
Mantra (vocals) ([S1], [S2]).

**Close competitors** (artist packs from other vendors): *unverified* in this sweep, because the search returned no
primary sources. Candidates for a follow-up: Toneforge (JST), STL Tones, Bogren Digital, IK AmpliTube artist
collections. Trade press frames signature plugins as deliberately narrow ([S24]), which argues against copying any
single product and for one general chain with good factory presets.

### 2.2 Signal chains per product

| Product | Special / pitch | Pre FX (series) | Amp → Cab → EQ | Post FX | Source |
|---|---|---|---|---|---|
| Plini X | n/a | Compressor → Octaver (±1 oct blend) → Drive (Fuzz switch) → Delay | 3 amps, DI BLEND, 9-band GEQ per amp | Chorus (80s/spatial) → Delay (tape sat + mod) → Reverb | [S5], [S9] |
| Gojira X | WOW pitch shifter (3 modes, expression, MIDI); OCT poly octaver (−1, −2) | OD → DRT → PHSR → CHR | 3 amps + Link, 9-band GEQ per amp | DLY (tape) → REV (dark shimmer) | [S6], [S9b] |
| Cory Wong X | Wah / auto-wah | Envelope filter → Compressor → Tube booster → Overdrive | 3 amps (one is a DI console with comp and HP/LP) + Link, room send, 9-band GEQ | Chorus → BBD delay → Reverb (shimmer switch) | [S7], [S10] |
| Nolly X | n/a | 2× OD/boost (bright sw) → solid-state comp → Delay | 4 amps + Link, 9-band GEQ ("custom Q" mode) | Delay → Reverb | [S8], [S11] |
| Petrucci X | Wah + Compressor | OD → Phaser/Vibe → Chorus → Flanger | 4 amps, room, 4-band semi-param EQ per amp, Volume | Stereo chorus → Stereo delay (cross-feed) → Reverb (shimmer) | [S14] |
| Tim Henson X | Multivoicer (4 voices, chords, MIDI mode) | Boost (bass/treble) → Comp (slow/fast) → OD | 3 amps, room, 9-band GEQ per amp | Stereo chorus → Delay (ping-pong/wide/normal × 3 types) → Reverb | [S13] |
| Rabea X | Synth (pre/post switch, arp, ladder filter) | Dual comp → Octaver → Fuzz → OD (vintage/modern sw) | 3 amps, room, 4-band semi-param EQ per amp | Stereo delay (pre/post sw) → Reverb (freeze, pitch) | [S15] |
| Misha Mansoor X | Laser (ring mod/pitch), Glitch (granular) | Comp → Tape echo → Dual octaver → Drive → Chaos dist | 3 amps (Voice sw, Boost) + 3 cabs, 7 mics, room, 9-band GEQ | Modulator (chorus/flanger) → Stereo delay (ducker) → Reverb (shimmer) | [S16] |
| John Mayer X | n/a | Boost → Envelope filter → OD → OD (2 modes) → short BBD delay | 3 parallel amps, harmonic tremolo + spring reverb unit, 11 mics, room S/M/L | Delay → Reverb (hall/plate) → 4-band EQ + Comp | [S17], [S19] |
| Mateus Asato | n/a | Opto comp (tilt EQ, mix) → OD → Dist/fuzz → Trem + chorus/vibe | 3 amps, 10 mics, 9-band GEQ per amp | Tape echo → Reverb (hall/plate, near-infinite) | [S17b] |
| Abasi | n/a | Lead-channel pedal → special comp | 3 amps (Clean "Blend", Lead booster), 9-band GEQ, multi-mic cab | Reverb + digital delay | [S12] |
| Tom Morello | Dive bomb (±12 st glide), Whammy-style (±24 st / ±100 c detune), Wah | FX loop: delay, 7-band EQ, phaser | 1 amp (2 ch), 10 mics + room, 9-band GEQ | Stereo delay → Reverb | [S18] |

### 2.3 Measured parameter ranges (from manual text)

| Block | Parameter | Range | Source |
|---|---|---|---|
| Pre delay (Plini X) | Time | 50–1000 ms; sync 1/64T…1/1D | [S5] |
| Pre delay (Nolly X) | Time | 50–1000 ms | [S8] |
| Post delay | Time | 7–6000 ms (Plini X), 7–3000 ms (Gojira X), 21–3000 ms (Nolly X), 100–1100 ms (Cory Wong X, BBD); sync 1/64T…1/1D | [S5], [S6], [S7], [S8] |
| Post delay | Low cut / High cut | 50–800 Hz or 60–500 Hz / 500 Hz–5 kHz | [S7], [S8] |
| Reverb | HPF / LPF | 50–700 Hz / 1–10 kHz | [S5], [S6] |
| Reverb | Decay | 1.0–8.5 s | [S8] |
| Phaser / Chorus | Rate | 0.12–10 Hz / 0.10–10 Hz | [S6] |
| Auto-wah | Attack / Decay | 10–1000 ms | [S7] |
| Envelope filter | Attack / Decay | 100–1000 ms | [S7] |
| DI amp (console) | HPF / LPF | 20–250 Hz / 2–17 kHz | [S7] |
| Transpose | Pitch | −12…+12 semitones | [S9]–[S16] |
| Clip LEDs | Hold | 10 s, click to clear | [S5] |

Per-amp knob sets (Gain, Bass, Mid, Treble, Presence, Depth, Master, Level and so on) are *unverified*: they are
artwork in the manuals. The text confirms only a per-amp LEVEL/OUTPUT ("volume without affecting tone") and a few
switches (Bright, Bite, Tight, Boost, Voice) ([S5], [S6], [S14], [S16]).

### 2.4 UX patterns

| Pattern | Detail | Source |
|---|---|---|
| Section tabs | Top icons (Pre FX, Amp, Cab, EQ, Post FX). Click to open. Right-click or double-click to bypass. | [S5], [S19] |
| Amp selector | Icons at the bottom. Switching the amp also switches its EQ. | [S5], [S6] |
| Global I/O | Input gain, Gate threshold, Output, peak meters with clip LEDs. "Input mode" (mono/stereo) is global. | [S5] |
| Preset manager | Factory + User tabs, folders and subfolders, asterisk when dirty, encrypted XML on disk | [S5] |
| Preset Explorer (2025+) | Search, tags, favourites, several hundred factory presets | [S16], [S17] |
| Global Lock (2025+) | Keeps Input, Gate, Transpose, Doubler and Output while browsing presets | [S16], [S17] |
| Live tuner | Always-visible strip (Ctrl+click toggles), plus a full tuner page | [S5] |
| Knobs | Drag up or down, double-click resets, Ctrl/Option for fine control. LCD fields accept typed values. | [S5] |
| Delay tap | Stomp switch tap sets time from the last two clicks | [S6], [S7] |
| MIDI | MIDI learn by right-click, a mapping table, CC/PC/Note, absolute/relative, savable mapping presets | [S5] |
| Window | Freely resizable. 16:10 layout from 2025. | [S5], [S16] |
| Blended macro | One macro page: input, amp blend, output, room | [S19] |

## 3. Recommendations

1. **Adopt the common skeleton as the v1 chain**, mapped onto MASTER_PROMPT: `Input/Gate → Comp → Drive/Boost (×2
   slots) → Amp (3 channels, per-channel EQ binding) → Cab (2 IR slots) → EQ (9-band graphic + HPF/LPF; parametric
   mode) → Modulation → Delay → Reverb → Transpose → Doubler → Output`.
2. **Amp:** 3 channels per preset (Clean, Crunch, Lead), each with Gain, Bass, Mid, Treble, Presence, Master and a
   per-channel Level (tone-neutral). Add one or two voicing switches (Bright, Tight). The knob set is our own choice,
   because the reference set is *unverified*.
3. **Cab:** 2 slots. Each slot has an IR (factory or user WAV), Level (−∞…+6 dB), Pan, Phase invert, and an
   on/off switch. Mic position and distance are only possible with multi-position IR sets, so for v1 use one
   **Position** selector over the IRs we can bundle (see the IR research ticket). Add an optional Room send later.
4. **EQ:** 9-band graphic at octave centres 63 Hz–16 kHz, ±12 dB, plus HPF 20–250 Hz and LPF 2–17 kHz (ranges as in
   [S7]). The band centres are our choice (*unverified* versus the reference).
5. **Delay:** 7–3000 ms free, sync 1/64T…1/1D, tap tempo, low cut 50–800 Hz, high cut 500 Hz–5 kHz, plus
   digital/analog(BBD)/tape modes and ping-pong. **Reverb:** decay 1–8.5 s (allow up to "freeze"), HPF 50–700 Hz,
   LPF 1–10 kHz, room/hall/plate/shimmer. **Modulation rate:** 0.1–10 Hz.
6. **Globals:** tuner with a live strip, Transpose ±12 st, Doubler, tap tempo, metronome (standalone only), MIDI learn
   (CC/PC/Note, absolute/relative), Global Lock, Undo/Redo, resizable UI, and section bypass by right-click.
7. **Preset schema:** keep input mode, tuner, metronome, MIDI map and locked globals **out** of presets. Store
   custom IR paths in a portable way, with relative paths and a hash fallback. Show a dirty marker.
8. **Defer** signature specials (whammy and dive bomb, poly octaver, harmonizer, synth, granular, ring mod, wah and
   envelope filter) to post-v1 tickets. Wah and octaver are the cheapest high-value ones.

## 4. Open questions (for the architect)

1. Should modulation be **position-switchable (pre/post amp)**, or a fixed post block plus an optional pre-amp
   modulation slot? Three references put phaser and chorus pre-amp.
2. Transpose and Doubler need a pitch shifter and a short modulated delay. What is the latency budget? A
   granular/PSOLA shifter adds latency that must go through `setLatencySamples`. Is it zero-latency only when off?
3. One active amp at a time, or **parallel amp blending** like the "three-in-one" amp in [S17] (2–3× amp CPU)?
4. Should we add a clean **DI blend** (parallel compressed DI around amp, cab and EQ, as in Plini X [S5])?
5. Should a preset hold all 3 channels (switchable by MIDI without a reload), or one channel per preset?
6. Should the "Global Lock" set (Input, Gate, Transpose, Doubler, Output) live in a separate global state file?

## 5. Sources

All accessed 2026-10-25.

- [S1] Neural DSP, Plugins listing: https://neuraldsp.com/plugins
- [S2] Neural DSP, Downloads (versions, dates, manual links): https://neuraldsp.com/downloads
- [S3] Wikipedia, "Neural DSP": https://en.wikipedia.org/wiki/Neural_DSP
- [S5] Archetype: Plini X user manual v1.0.0 (PDF): https://downloads.neuraldsp.com/file/archetype-plini-x-installers/v_1.0.0/Archetype-Plini-X-v1.0.0.pdf
- [S6] Archetype: Gojira X user manual v1.0.0 (PDF): https://downloads.neuraldsp.com/file/archetype-gojira-x/Archetype%20Gojira%20X%20v1.0.0.pdf
- [S7] Archetype: Cory Wong X user manual v1.0.0 (PDF): https://downloads.neuraldsp.com/file/archetype-cory-wong-x/Archetype%20Cory%20Wong%20X%20v1.0.0.pdf
- [S8] Archetype: Nolly X user manual v1.0.0 (PDF): https://downloads.neuraldsp.com/file/archetype-nolly-x-installers/Archetype%20Nolly%20X%20v1.0.0.pdf
- [S9] Product page, Plini X: https://neuraldsp.com/plugins/archetype-plini
- [S9b] Product page, Gojira X: https://neuraldsp.com/plugins/archetype-gojira
- [S10] Product page, Cory Wong X: https://neuraldsp.com/plugins/archetype-cory-wong
- [S11] Product page, Nolly X: https://neuraldsp.com/plugins/archetype-nolly
- [S12] Product page, Abasi: https://neuraldsp.com/plugins/archetype-abasi
- [S13] Product page, Tim Henson X: https://neuraldsp.com/plugins/archetype-tim-henson
- [S14] Product page, Petrucci X: https://neuraldsp.com/plugins/archetype-petrucci
- [S15] Product page, Rabea X: https://neuraldsp.com/plugins/archetype-rabea
- [S16] Product page, Misha Mansoor X: https://neuraldsp.com/plugins/archetype-misha-mansoor
- [S17] Product page, John Mayer X: https://neuraldsp.com/plugins/archetype-john-mayer
- [S17b] Product page, Mateus Asato: https://neuraldsp.com/plugins/archetype-mateus-asato
- [S18] Product page, Tom Morello: https://neuraldsp.com/plugins/archetype-tom-morello
- [S19] Sound On Sound, "Neural DSP Archetype: John Mayer X" review (Apr 2026): https://www.soundonsound.com/reviews/neural-dsp-archetype-john-mayer-x
- [S20] Neural DSP news, "Introducing Archetype: Misha Mansoor X": https://neuraldsp.com/news/introducing-archetype-misha-mansoor-x
- [S21] Premier Guitar, "Neural DSP Unveils Archetype: Cory Wong": https://www.premierguitar.com/neural-dsp-unveils-archetype-cory-wong
- [S22] Gearspace, "Neural DSP releases Archetype: Tim Henson": https://gearspace.com/threads/neural-dsp-releases-archtype-tim-henson.1357878/
- [S23] Sonicstate, "John Petrucci's Guitar Sound In Software" (2021-12-24): https://sonicstate.com/news/2021/12/24/john-petruccis-guitar-sound-in-software
- [S24] Sound On Sound, "Neural DSP Archetype: Tom Morello" review: https://www.soundonsound.com/reviews/neural-dsp-archetype-tom-morello
