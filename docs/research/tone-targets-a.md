# Tone targets A: clean, ambient and prog lineup

- **Issue:** #4 · **Author:** acct1/researcher (Lyra) · **Date:** 2026-10-25
- **Scope:** Plini, Tosin Abasi, Cory Wong, Tim Henson, Rabea Massaad. High-gain techniques (tight boost, gate,
  palm-mute HPF, post-cab cuts) are covered in tone-targets-b (#5) and not repeated here.
- **Internal only.** The wording "aims at a tone similar to X" never leaves `docs/research/` (AGENTS.md rule 2).
  All factory presets get original names.
- **Method:** public product pages, public PDF manuals (text layer), rig rundowns, reviews, interviews. Nothing was
  downloaded from or run against a commercial plugin. Some pages blocked fetches (ploutone.com 429,
  producelikeapro.com 403, musicradar.com truncated). Claims taken only from a search-result snippet are tagged
  *(snippet)*. **No source publishes knob-by-knob artist settings for these five**, so every numeric target in §3 is
  an *unverified* engineering hypothesis. The preset designer must confirm it by measurement (§3.4).

## 1. Summary (for the architect and preset designer)

1. **Four of the five targets sit in clean to mid-gain territory.** Gain-structure coverage needs: a hi-fi clean
   (pristine, compressed), an edge-of-breakup clean, a low/mid-gain lead (comp → low-gain OD → crunch amp), and one
   high-gain rhythm (Abasi, Rabea). Ambient delay and reverb carry Plini's and Henson's identity more than amp gain
   does (S1, S5, S9).
2. **The compressor is the first block and part of the tone, not a utility.** Cory Wong's whole board starts with an
   optical-style comp (S6, S7). Plini runs "quite a bit of compression at the very front" *(snippet, S2)*. Henson
   tracks with a "slightly compressed clean" (S11). Rabea has a two-stage comp (S14). → Ship a comp with attack,
   release, blend (parallel) and a dual-stage mode.
3. **A DI/console "amp" without a cab is needed** for the Wong funk tone: tube saturation + comp + EQ + HPF 20–250 Hz /
   LPF 2–17 kHz, usable cab-less (S4, S6). → Model it as an amp-slot "console" type with the cab auto-bypassed.
4. **An acoustic/piezo voicing appears in 2 of 5** (Henson acoustic amp, Abasi Clean "Blend" tube↔piezo) (S9, S12). →
   One "piezo-style" EQ voicing (body resonance ~100–250 Hz, scooped 1–3 kHz, airy 6–10 kHz) blendable with the
   clean amp is cheaper than a separate amp model.
5. **Pitch effects:** a polyphonic octave up/down (Plini, Rabea, Wong's octave pedals) and a **4-voice harmonizer**
   (Henson) (S1, S9, S14). The octave can share the poly-octave block from tone-targets-b. The harmonizer is post-v1
   (open question 1).
6. **Spectral targets are given per octave band (63 Hz–8 kHz, dB relative to 1 kHz)** in §3.3 for all 16 preset types,
   with gain-staging numbers in §3.4. They are starting hypotheses built from the vendor's EQ guide (S16) and the
   plugin ranges, *unverified* until rendered.
7. **Add 30 names to `tools/guard/forbidden_names.txt`.** Generic single words (Ego, Kraken, Optimist, Clean
   Machine, Logos) were *not* added because they would false-positive on product code.

## 2. Findings

### 2.1 Reference-plugin structure (what each inspiration's signature plugin exposes)

| Inspiration | Amps | Pre-FX | Cab | Post-FX / extras | Src |
|---|---|---|---|---|---|
| Plini | 3: Clean, Crunch, Lead (Lead = Crunch + a triode stage). Amp BLEND adds a **compressed DI** that skips everything but the post FX | Comp (threshold linked to makeup) → Octaver (±1 oct) → Drive (fuzz sw) → Delay 50–1000 ms | IRs, 6 mics, user IR | 9-band GEQ per amp. Chorus (80s/spatial) → Delay 7–6000 ms (tape sat + mod) → Reverb (HPF 50–700 Hz, LPF 1–10 kHz). Transpose, Doubler | S1, S3 |
| Abasi | 3: Clean (Blend: tube ↔ piezo-like), Rhythm, Lead (built-in booster) | Pathos (lead channel as a pedal, also stacks on Clean), Logos (comp for "thump") | 70+ IRs, multi-mic | 9-band GEQ. Reverb + digital delay | S12, S13 |
| Cory Wong | 3: D.I. console (tube sat, comp, selectable EQ, HPF 20–250 Hz / LPF 2–17 kHz, cab-less), clean, edge-of-breakup (with power-amp stage) | Wah/auto-wah (A/D 10–1000 ms) → envelope filter (A/D 100–1000 ms) → comp → tube boost → OD | 324 IRs, 6 mics, stereo 2nd mic, room send, link | 9-band GEQ + HPF/LPF. Chorus → BBD delay 100–1100 ms (low cut 50–800 Hz, high cut 0.5–5 kHz) → reverb with shimmer | S4, S6 |
| Tim Henson | 3: acoustic (blend), British low-gain (2 ch, 3-band + presence), lead | Multivoicer (4 voices, chords, MIDI) → Boost (bass/treble) → Comp (slow/fast) → OD | 324 IRs, 6 mics, stereo, room | 9-band GEQ + HPF/LPF per amp. Stereo chorus → Delay (ping-pong/wide/normal; diffusion/vintage digital/modern) → Reverb | S9, S10 |
| Rabea | 3: Clean ("Fender-like", lots of attack), Crunch/Rhythm (master-volume rock head), Lead (classic rock → Scandinavian high-gain). EL34/6L6 choice. Mid/high boost switches, Depth/Tight | Dual comp → Octaver (up/down) → Fuzz → OD (vintage/modern sw). Synth (pre/post) | 3 cabs, dual mic, user IR, room | 4-band semi-param EQ + HPF/LPF. Dual/stereo delay → Reverb (freeze). No modulation | S14, S15 |

### 2.2 Artist rigs (where a primary or rig source exists)

| Inspiration | Guitar / pickups | Amp | Comp / drive | Time FX | Src |
|---|---|---|---|---|---|
| Plini | Headless 6-string with HSS-style humbucker + single-coil set, volume only (no tone knob) | Modellers only, "never owned a tube amp". Uses HBE/BE-type models *(forum, secondhand)* | Heavy comp first, then a clean-boost low-gain OD *(snippet, S2)* | Lead = "moderate delay, a fat dollop of reverb" *(snippet, S2)*. Big fan of delay for dimension *(snippet)* | S2, S5, S17 |
| Abasi | 8-string, standard tuning with the low string dropped to E. Fanned-fret signature models | 2013: modeller into a tube head + 2×12. 2017: tube heads + pedals. Now: an EL34 high-gain head with a built-in gate, run stereo | Signature distortion (3-band EQ, dark/bright voicing) | n/a | S18, S19, S20 |
| Cory Wong | Strat-style, single coils, 5-way (push/push forces position 4) | 2× solid-state heads with a tube preamp stage (250 W), 2 cabs: 1×12 vintage-style and 1×12 EVM-type | **Optical-style comp first** (Volume, Sustain, Attack, Blend, Tone). Signature version adds a boost with a ~550 Hz mid-lift mode | Reverb (no delay listed). Volume/wah, envelope filter, octave fuzz, 2 ODs, dual octave | S6, S7, S8 |
| Tim Henson | 6-string with multi-voice active pickups. Neck "hyper clean" voice used as an acoustic/nylon surrogate | Tracks everything through his own signature plugin, using a preset he calls "the most basic dry tone" (S11b) | Lesson clean: gain 3, bass 7, mid 6, treble 7, reverb 2, "slightly compressed clean" (lesson, not stage rig) | Light reverb | S11, S11b |
| Rabea | n/a | Signature high-gain amp line (UK maker). A preamp pedal version exists | Precision-style drive, micro pitch shifter, wah *(fan gear list)*. Signature distortion pedal | n/a | S14, S21, S22 |

### 2.3 Technique specifics

**T1. Parallel DI blend (Plini X).** The amp BLEND adds a compressed copy of the DI that bypasses amp, cab and EQ and
rejoins before the post FX (S3). It restores pick attack and low-end definition on mid-gain leads. → A
"clean blend" parameter (0–100 %) with a fixed fast comp (*unverified*: ratio 4:1, attack 5 ms).

**T2. Funk clean compression.** An optical-style comp with an attack control and a **blend** (parallel) knob. A
reviewer at noon on everything hears it as "squashy" with an audible "mmppff" attack *(snippet, S7b)*. → Target 6–10 dB
GR on 16th-note chops, attack 10–30 ms so the pick transient survives, release ~100–200 ms, blend 60–80 %
(*unverified*).

**T3. Studio bus comp as the reference.** Wong names a stereo VCA bus compressor as his favourite studio comp *(snippet,
S7c)*. → A "glue" mode on the output comp (2:1, slow attack 30 ms, auto release) is worth an EQ+comp output block
(also needed by the newest lineup products, see reference-plugins §2.2).

**T4. Clean amp settings (Henson lesson).** gain 3 / bass 7 / mid 6 / treble 7 / reverb 2 (on 0–10 scales). Bass and
treble sit above mid, so the clean is mildly scooped and bright, with light compression (S11). Use it as the starting
point for the low-gain British channel.

**T5. Generic EQ moves (vendor guide).** Clean: +2…+4 dB @ 80 Hz, +1…+3 dB @ 800 Hz, +1…+2 dB @ 6 kHz, cut > 10 kHz.
Classic distortion: +2…+4 @ 100 Hz, −2…−3 @ 400 Hz, +3…+6 @ 3 kHz. Metal: +3…+5 @ 80–100 Hz, −3…−5 @ 250–500 Hz,
+3…+5 @ 3 kHz, leads +4…+6 @ 5 kHz. Low cut below 70 Hz (S16). This is the base for the §3.3 band tables.

## 3. Recommendations

### 3.1 DSP blocks this lineup needs (in addition to tone-targets-b §3.1)

| Block | Parameters / ranges | Algorithm / note | License |
|---|---|---|---|
| Compressor (pre, first block) | thr −50…0 dB, ratio 1.5–20, attack 0.1–100 ms, release 20–1000 ms, blend 0–100 %, makeup auto/manual, mode: opto / VCA / dual-stage | Feed-forward, log-domain detector. "Opto" = program-dependent release (two-stage release). Dual-stage = 2 comps in series with different attack | Own code |
| Envelope filter / auto-wah | attack 10–1000 ms, decay 10–1000 ms, sensitivity, range 200 Hz–3 kHz, Q 1–10, LP/BP | Envelope → TPT SVF cutoff, smoothed | Own code |
| Console amp type | input drive, tube sat 0–1, comp amount, 3-band EQ + HPF 20–250 Hz, LPF 2–17 kHz | Waveshaper (asymmetric soft clip, 2× OS) + comp + biquads; cab bypass flag | Own code |
| Clean DI blend | 0–100 %, fixed comp | Parallel path amp-in → post-FX, latency-aligned with the amp/cab path | Own code |
| Piezo voicing | blend 0–100 % with clean amp | Static EQ (§1 item 4), min-phase | Own code |
| Poly octave ±1 | up / down / dry levels | Shared with tone-targets-b poly-octave block | TBD (#11) |
| Harmonizer (post-v1) | 4 voices, interval/scale/key, MIDI mode | PSOLA or phase vocoder; latency 10–40 ms | TBD (#11) |
| Chorus | rate 0.1–10 Hz, depth, mix, mode 80s (1 voice, BBD-style, dark) / spatial (stereo 2–3 voices) | Modulated delay 5–25 ms, LPF in the BBD mode | Own code |
| Delay | 7–6000 ms, sync, fb 0–100 %, mix, low cut 50–800 Hz, high cut 0.5–5 kHz, modes digital/BBD/tape, ping-pong/wide, ducking | Fractional delay (cubic), saturation + wow/flutter in the tape mode | Own code |
| Reverb | decay 0.3–20 s + freeze, pre-delay 0–200 ms, HPF 50–700 Hz, LPF 1–10 kHz, shimmer +12 st mix | FDN 8–16 lines; shimmer = pitch-shifted feedback | Own code |

### 3.2 Target preset types (original names chosen later by the preset designer)

| ID | Inspiration ("aims at a tone similar to") | Preset type | Key settings |
|---|---|---|---|
| A1 | Plini | Crystalline ambient clean | Clean amp, comp 4–6 dB GR, chorus spatial, delay ♩. 30 % fb 25 % mix, hall 4–6 s 30 % mix |
| A2 | Plini | Singing mid-gain lead | Comp → low-gain OD (drive low, level high) → Crunch/Lead amp, mid +2 dB, delay ♩. 35 % fb 20 %, reverb 3 s 25 %, DI blend 15 % |
| A3 | Plini | Fuller jazzy chord clean | Neck pickup, Clean amp, little comp, plate 2 s 15 %, no delay |
| A4 | Abasi | Thumpy clean (slap/thump technique) | Clean amp, "thump" comp (attack 20–30 ms, 6 dB GR), piezo blend 30 %, short room |
| A5 | Abasi | 8-string djent rhythm | Lead amp + tight boost (see tone-targets-b F1–F3), HPF 90 Hz, gate (tone-targets-b §3.3) |
| A6 | Abasi | Ambient piezo-blend clean | Blend max, delay dotted-♪, reverb 6 s 35 % |
| A7 | Cory Wong | Funk chop clean (DI console) | Console amp, comp 8–10 dB GR, attack 20 ms, blend 70 %, HPF 100 Hz, no cab, dry |
| A8 | Cory Wong | Position-4 Strat clean through amp | Clean amp, comp 6 dB, short room 0.8 s 10 % |
| A9 | Cory Wong | Edge-of-breakup with boost | Edge amp, comp → tube boost +6 dB, spring/plate 1.5 s |
| A10 | Cory Wong | Envelope-filter funk | Env filter BP, Q 5, range 300 Hz–2.5 kHz, attack 10 ms, decay 300 ms |
| A11 | Tim Henson | Dry "basic" clean (tracking tone) | British low-gain amp (T4 settings), comp slow 3–4 dB, reverb 1.2 s 8 % |
| A12 | Tim Henson | Acoustic-sim / nylon surrogate | Piezo voicing 100 %, comp, plate 1.5 s 15 %, stereo chorus subtle |
| A13 | Tim Henson | High-gain lead with stereo delay | Lead amp, boost, ping-pong delay ⅛. 30 % fb 20 % |
| A14 | Rabea | Tight modern high-gain rhythm | Lead amp, Tight up, EL34, OD (modern) boost, HPF 90 Hz |
| A15 | Rabea | Octave-fuzz heavy riff | Octaver −1 → fuzz → Crunch amp |
| A16 | Rabea | Freeze-reverb ambient clean | Clean amp, dual comp, stereo delay, reverb freeze |

All values in this table are *unverified* starting points (no artist knob settings are published, see Method).

### 3.3 Spectral targets: long-term average spectrum, dB per octave band, relative to the 1 kHz band

Build: the vendor guide's band moves (T5) applied on top of a typical guitar-cab long-term spectrum, then rounded to
1 dB. Boost = level above unity at drive ≈ 0, GR = gain reduction on DI peaks, gate threshold relative to the
−12 dBFS DI reference ("—" = off), the same conventions as tone-targets-b §3.3.
**All rows are *unverified*.** They are tolerance centres for the first preset pass (±3 dB per band), to be replaced
by measured values from §3.4.

| ID | 63 | 125 | 250 | 500 | 1k | 2k | 4k | 8k | Boost dB | Comp GR dB | Gate thr dBFS |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A1 ambient clean | −12 | −3 | −1 | −1 | 0 | −2 | −5 | −11 | — | 5 | — |
| A2 mid-gain lead | −16 | −6 | −2 | 0 | 0 | −1 | −5 | −14 | +8 | 4 | −60 |
| A3 jazzy clean | −10 | −1 | +1 | 0 | 0 | −4 | −9 | −18 | — | 2 | — |
| A4 thumpy clean | −8 | 0 | −1 | −2 | 0 | −2 | −4 | −10 | — | 6 | — |
| A5 8-str djent | −13 | −4 | −4 | −3 | 0 | +1 | −2 | −12 | +10 | — | −50 |
| A6 piezo ambient | −9 | −1 | 0 | −2 | 0 | −2 | −3 | −7 | — | 3 | — |
| A7 funk DI chop | −18 | −8 | −3 | −1 | 0 | 0 | −3 | −9 | — | 9 | — |
| A8 Strat clean | −14 | −5 | −2 | −1 | 0 | −1 | −4 | −10 | — | 6 | — |
| A9 edge-of-breakup | −14 | −5 | −1 | 0 | 0 | −2 | −6 | −14 | +6 | 6 | — |
| A10 envelope funk | −16 | −5 | −1 | +1 | 0 | −1 | −6 | −15 | — | 6 | — |
| A11 dry clean | −13 | −3 | −2 | −2 | 0 | −1 | −3 | −10 | — | 3 | — |
| A12 acoustic-sim | −9 | −1 | 0 | −3 | 0 | −3 | −3 | −6 | — | 4 | — |
| A13 hi-gain lead | −15 | −6 | −3 | −1 | 0 | 0 | −2 | −13 | +8 | — | −55 |
| A14 hi-gain rhythm | −12 | −3 | −4 | −3 | 0 | +1 | −1 | −12 | +10 | — | −50 |
| A15 octave fuzz | −8 | 0 | −1 | −2 | 0 | −2 | −6 | −15 | — | — | −58 |
| A16 freeze clean | −11 | −3 | −1 | −1 | 0 | −2 | −5 | −12 | — | 5 | — |

### 3.4 Gain staging and measurement procedure

| Stage | Target (*unverified* defaults) |
|---|---|
| DI input (factory DI clips) | Peaks −12 dBFS (humbuckers), about −15 dBFS (single coils), RMS ≈ −24 dBFS on strummed chords. Same reference as tone-targets-b §3.3. All gain settings assume it |
| Compressor | GR as given per preset (3–10 dB), measured on the reference DI clip |
| Boost / OD into amp | Level +6…+10 dB, drive ≤ 20 % for "clean-boost low-gain OD" (A2, A9, A13, A14) |
| Amp output | Peaks −6…−3 dBFS before post FX (headroom for delay and reverb build-up) |
| Wet mixes | Delay mix measured as wet/dry energy ratio at the first repeat. Reverb mix as late-tail level 500 ms after a staccato chord, −20…−10 dB relative to the dry peak |
| Preset output | **−18 LUFS-I** integrated over the reference DI clip, ±1 LU. Lead presets (A2, A13) −17 LUFS-I. Same as tone-targets-b and the `nam-runtime.md` −18 dB convention |

Procedure for the preset designer:
1. Render each preset with `osp-render` over the same DI clip set (single-note lead, chords, 16th-note chops).
2. Compute the octave-band LTAS (Welch, 8192-pt, Hann, 50 % overlap, sum power within each octave band), normalize
   to the 1 kHz band, and compare with §3.3 (±3 dB tolerance).
3. Measure LUFS-I (ITU-R BS.1770-4), and normalize via the output level to −18 LUFS (leads −17).
4. Commit the measured table back into this note (or `presets/targets.json`) and replace the hypotheses.

## 4. Open questions (for the architect)

1. Harmonizer (4-voice, MIDI-driven): in scope for v1, or post-v1? It is needed for one inspiration only, but it is the
   most recognizable feature of that plugin. It also has latency implications (coordinate with #11).
2. Should "console" be an amp-slot type that auto-bypasses the cab, or a separate block?
3. Should the clean DI blend be a per-amp parameter or a global parallel path? It needs latency alignment with any
   oversampling inside the amp.
4. Should the piezo voicing be an EQ curve blended with the clean amp, or a dedicated acoustic-body IR (requires
   licensable IRs, see the IR research ticket)?
5. Where do the spectral targets live: in this note, or in a machine-readable `presets/targets.json` that the QA
   golden-render tests read?
6. LUFS target: tone-targets-b and this note both use −18 LUFS-I with leads at −17. Should ambient cleans also get their own
   offset (they read quieter at equal LUFS because of the long tails)?

## 5. Sources (all accessed 2026-10-25)

| ID | URL |
|---|---|
| S1 | https://neuraldsp.com/plugins/archetype-plini |
| S2 | https://ploutone.com/blogs/hone-your-tone/how-to-tone-episode (429 on fetch, search snippet only) |
| S3 | https://downloads.neuraldsp.com/file/archetype-plini-x-installers/v_1.0.0/Archetype-Plini-X-v1.0.0.pdf |
| S4 | https://downloads.neuraldsp.com/file/archetype-cory-wong-x/Archetype%20Cory%20Wong%20X%20v1.0.0.pdf |
| S5 | https://producelikeapro.com/blog/who-is-plini-the-future-of-exceptional-guitar-playing/ (search snippet) |
| S6 | https://www.premierguitar.com/videos/rig-rundown/cory-wong (2022-03-02) |
| S7 | https://www.wamplerpedals.com/cory-wong/ (search snippet) |
| S7b | https://www.guitarsrebellion.com/en/products/wampler-cory-wong-compressor-boost (reviewer settings, search snippet) |
| S7c | https://uaudio.com/blog/clean-guitar-tones-with-cory-wong (search snippet) |
| S8 | https://neuraldsp.com/plugins/archetype-cory-wong |
| S9 | https://neuraldsp.com/plugins/archetype-tim-henson |
| S10 | https://www.guitarworld.com/news/neural-dsp-archetype-tim-henson (search snippet) |
| S11 | https://money.yahoo.com/5-mind-blowing-techniques-learn-153056073.html (lesson settings, search snippet) |
| S11b | https://killerguitarrigs.com/polyphias-tim-henson-reveals-lazy-aspect-of-his-studio-work-explains-how-they-recreate-weird-guitar-tones-live/ (search snippet) |
| S12 | https://neuraldsp.com/plugins/archetype-abasi |
| S13 | https://unity.neuraldsp.com/t/archetype-abasi-presets-thread/222 (forum, search snippet) |
| S14 | https://www.soundonsound.com/reviews/neural-dsp-archetype-rabea (Nov 2022) |
| S15 | https://neuraldsp.com/plugins/archetype-rabea |
| S16 | https://neuraldsp.com/articles/electric-guitar-eq-guide |
| S17 | https://forum.fractalaudio.com/goto/post?id=1545610 (forum, secondhand, search snippet) |
| S18 | https://www.premierguitar.com/gear/rig-rundown-animals-as-leaders-tosin-abasi-and-javier-reyes-2013 (search snippet) |
| S19 | https://www.premierguitar.com/gear/rig-rundown-animals-as-leaders-2017 (search snippet) |
| S20 | https://www.guitarworld.com/news/tosin-abasi-bad-cat-amps (search snippet) |
| S21 | https://equipboard.com/pros/rabea-massad (fan-compiled, search snippet) |
| S22 | https://www.guitarpedalx.com/news/gpx-blog/victory-amps-v1-the-kraken-high-gain-preamp-pedal-goes-well-beyond-its-rabea-massaad-signature-sound-roots (search snippet) |
