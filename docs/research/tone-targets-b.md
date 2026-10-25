# Tone targets B: high-gain and the rest of the lineup

- **Issue:** #5 · **Author:** acct3/researcher (Wren) · **Date:** 2026-10-25
- **Scope:** the inspirations that tone-targets-a (#4) does not cover: Gojira (Joe Duplantier), Nolly (Adam Getgood),
  John Petrucci, Misha Mansoor, Tom Morello, John Mayer, Mateus Asato. Plus the shared high-gain techniques:
  tight boost, noise gate, palm-mute tightness, presence/resonance, post-cab EQ cuts.
- **Internal only.** The wording "aims at a tone similar to X" never leaves `docs/research/` (AGENTS.md rule 2).
  All factory presets get original names.
- **Method:** public product pages, launch news, reviews and rig articles only. Nothing was downloaded from or run
  against a commercial plugin. Several rig sites (reverb.com, musicradar.com, electrosmash.com) blocked or truncated
  direct fetches. Claims taken only from a search-result snippet of those pages are tagged *(snippet)*: a reviewer
  should treat them as weaker than a fully read page.

## 1. Summary (for the architect and preset designer)

1. **The lineup converges on one signal chain** (S1–S7): input gate → comp → 2 drives (one TS-style boost) →
   3–4 amp channels (clean / crunch / rhythm / lead) → per-amp graphic EQ (9-band, or 4-band semi-parametric with
   HPF/LPF) → cab IR with matched/unlinked cab and movable mics → delay, reverb (often a dark shimmer), doubler,
   transpose. Our graph needs nothing beyond what tone-targets-a already implies, **plus a polyphonic octave-down
   (−1/−2 oct) and a whammy-style pitch shifter** (S1, S5).
2. **The tight boost is a filter, not a distortion.** Model a TS-style stage with an input HPF near **720 Hz** inside
   the clipping loop and a treble roll-off. Preset it at **drive ≈ 0, level ≈ max, tone ≈ noon** in front of the
   high-gain channels (F1, F2). Ship a 6-position "attack" (pre-HPF) switch as well (F3).
3. **Palm-mute tightness = pre-amp HPF 80–120 Hz (steep) + boost mid-hump.** Gentler HPF for 7/8-string presets
   (F2, F7). Make the HPF corner a preset parameter, range 20–300 Hz.
4. **Noise gate:** first block, keyed from the clean input, with hysteresis. Defaults are *unverified* engineering
   starting points: threshold −55 dBFS (range −90…−20), attack 0.5 ms, hold 20 ms, release 80 ms, hysteresis 4 dB.
   QA must calibrate them against the DI clips' measured noise floor (§3.3).
5. **Post-cab EQ template for high-gain rhythm:** HPF 80–120 Hz, cut mud at 200–500 Hz (narrow), find and cut the
   resonance at 3–5 kHz (narrow), LPF 10–12 kHz or a high shelf from 8–10 kHz for fizz (F7).
6. **Mark-style 5-band graphic EQ** (80 / 240 / 750 / 2200 / 6600 Hz, wide Q) is worth shipping as an EQ "voicing"
   option. It is the main tool for the Petrucci-style scooped lead/rhythm sound (F5).
7. **Clean/edge targets** (Mayer, Asato, the Gojira clean) need: an always-on clean boost, a Klon-style low-gain
   drive, an opto compressor, spring reverb + tremolo, slapback (~80–120 ms) and a 1×15/1×12 open-back cab (S6, S7).
8. **Add 61 names to `tools/guard/forbidden_names.txt`** (see the PR). Generic words (Orange, Boss, CTS, Victoria)
   were *not* added because they would false-positive on product code. Reviewers check them by hand.

## 2. Findings

### 2.1 Reference-plugin structure (what each inspiration's signature plugin exposes)

| Inspiration | Amps (count: names) | Pre-FX | Cab | Post-FX / extras | Src |
|---|---|---|---|---|---|
| Gojira | 3: Clean (vintage tube), Rust (crunch, "one of the biggest gain sweeps"), Hot ("clear, defined, ultra high-gain") | OD, DRT (distortion), PHSR, CHR. Pitch: WOW (3-mode MIDI pitch shifter), OCT (polyphonic, −1 and −2 oct) | Hundreds of IRs, 6 movable mics, link/unlink, user IR | 9-band GEQ per amp. Delay with tape saturation. "Chasmic" reverb with dark shimmer. Transpose ±12, Doubler, tuner | S1, S2 |
| Nolly | 4: Clean, Crunch ("British crunch with a twist"), Rhythm ("full and massive"), Lead (different high-gain flavour) | 2 OD/boosters (OD-1 has a bright switch, "leave this on unless you want a smooth character"), solid-state comp, delay | 640 IRs by the artist, 4 speaker options, link/unlink | 9-band GEQ + "mod" GEQ with custom Q. Reverb, delay, room reverb (X update) | S3, S4 |
| Petrucci | 4: Piezo (acoustic body), Clean (bright sw), Rhythm (tight knob, bite sw, bass knob), Lead | Wah, comp, OD, phaser/vibe, chorus, flanger | IRs, 6 movable mics, room control, user IR | 4-band semi-parametric EQ + HPF/LPF per amp. Stereo chorus, stereo delay (cross-feed, tape sat.), reverb with shimmer | S5 |
| Misha Mansoor | 3: Clean, Rhythm, Lead (gain rises across them; low end "controlled" for 7/8-string) | Comp, tape delay, OD (model of his signature low-gain drive), octaver, distortion ("Chaos": custom DIY pedal inspired by the Swedish "chainsaw" sound) | 3 matched cabs, IRs by Nolly, user IR | 9-band GEQ after amp+cab. Laser (ring-mod + pitch), Glitch (granular). Stereo delay with ducker, reverb w/ shimmer, modulator | S6, S11 |
| Tom Morello | 1 amp: 50 W 2205-style head, single boost channel | Phaser, EQ-as-boost, 2 delays, whammy (Dive Bomb ±12, Wham ±24 st, detune), wah, auto-pitch | 4×12 with 85 W speakers. 2 of 10 mics at once, user IR | Tuner, doubler, transpose | S7, S8 |
| John Mayer | 3 + blend: '64 blackface 1×15 combo, boutique "steel string singer" head, signature prototype head, "3-in-1" mix | Clean boost (always on), envelope filter, Klon-style OD, dual-mode TS-10 / Bluesbreaker OD, short analog slapback, spring reverb + tremolo | 1×15 CTS, 1×12 EVM12L, 1×12 Greenback | Digital delay, hall/plate reverb, 4-band semi-param EQ + HPF/LPF, studio compressor | S9, S10 |
| Mateus Asato | 3: British edge-of-breakup ("tighter, brighter, punchier"), Fender-style modern clean, high-gain lead | Opto comp, 2 ODs, trem/chorus/vibrato | Hundreds of IRs, 10 mics, user IR | 9-band GEQ per amp. Tape-style 80s delay, reverb | S12 |

**Gate:** none of the product pages lists a gate module. One review places a continuously adjustable gate in the
header next to the input/output knobs *(snippet, S13)*. Treat "global gate in the I/O header" as the convention.

### 2.2 Artist rigs (where a primary or rig source exists)

| Inspiration | Amp | Boost / drive | Gate | Cab / other | Src |
|---|---|---|---|---|---|
| Gojira | 5150 III, EL34 and 6L6 versions, split after the pedals. EL34 → stage cabs, 6L6 → load box/cab sim → FOH | **Conflicting.** A community capture uses a TS808 boost (secondary). A rig interview says there is no drive pedal; the tone comes from the amp *(snippet)* | Smart Gate Pro in front of the amp *(snippet)* | Hall reverb + 2 delays in the loop, whammy *(snippet)*. Drop D / Drop C tunings (secondary) | S14, S15, S16, S17 |
| Nolly | Modified amps (models not named publicly). Not verified as 5150-based | Two boosters. A tutorial: Amp 3 + red booster "to tighten the low end" | — | Interview: saturated single notes often "fall apart" when palm-muted, so tune gain for the mute, not the sustain *(snippet)* | S3, S4, S18, S19 |
| Petrucci | Mark series (Mark V in 2011, signature JP-2C later). JP-2C ch. 2 is voiced for the "classic Mark IIC+ grinding rhythm" | An OD on the board that changes often. Compressor-limiter. Later: a signature-style red OD | — | Wah, PH-3 phaser, flanger, Axe-FX in the loop (2011). JP-2C: 2 assignable 5-band GEQs, 2 presence voicings | S20, S21, S22 |
| Misha Mansoor | Fractal live. Earlier tube head with built-in gate | Precision Drive: **drive ≈ 0, volume high**, 6-position "attack" switch that progressively tightens the lows | Drive pedal gate **and** amp gate both on | Juggernaut 6-string with passive humbuckers | S11 |
| Tom Morello | JCM800 2205 50 W, boost channel only. Presence 7, Bass 10, Middle 10, Treble 7, Gain 9 (volumes listed as 0 and 6, ambiguous) | **Flat 7-band EQ pedal used as a level boost** (100 Hz–6.4 kHz, ±18 dB) | — | 4×12 G12K-85. Two DD-3 delays (12.5–800 ms; one long for leads, one slapback/ping-pong). Whammy at +5th harmony | S8 |
| John Mayer | See 2.1 (the plugin models his personal amps) | Always-on clean boost → Klon-style → TS-10 / Bluesbreaker | — | Users report the default presets are "excessively bass-heavy" and voiced for single coils *(aggregated user reports)* | S9, S10, S23 |
| Mateus Asato | See 2.1 | 2 OD flavours, opto comp | — | — | S12 |

### 2.3 High-gain technique specifics

**F1. TS-style boost circuit.** In the clipping stage, a 0.047 µF cap + 4.7 kΩ in series form a **~720 Hz high-pass**:
only content above ~720 Hz gets full gain and distortion. The 51 pF feedback cap rolls off the treble. The minimum
drive path is 51 kΩ, with a zero near 66 Hz at minimum drive. Raising the 0.047 µF cap reduces the mid-hump. All of
this is from hobbyist analyses (eCircuit Center, diystompboxes SPICE threads), but they agree (S24, S25). → Model it
as a WDF/nodal op-amp clipper, or as an EQ (HPF ~720 Hz, 1st-order) → soft clipper → 1st-order LPF. **To verify:**
SPICE the stock circuit in `tools/` and compare.

**F2. Why "gain 0, level max".** The pedal's mid-hump balances the bottom-heavy high-gain amps, so the lows tighten
and the upper mids stay focused. The usual setting is level high, gain low, and the amp gain down a little (S26). It
is placed before the amp, after the tuner/wah/comp (S27). Counterpoint: some modelled TSs need a little drive above 0
(S28). An alternative is a pre-amp EQ (HPF + mid boost + presence) (S26, S29).

**F3. "Attack" switch.** The Precision Drive's six positions progressively tighten the low end for down-tuned guitars
(S11). → Implement a 6-step pre-boost HPF. Suggested corners (*unverified*): 40, 60, 80, 100, 130, 170 Hz, 2nd order.

**F4. Noise gate.** Gates pass the signal only above a threshold and are standard for guitar hum and hiss (S30).
Users report that thresholds set too high make the guitar sound "weak". A suggested fix is a lower input level into
the sensitive amp models (S13, forum). Rigs: Smart Gate in front of the amp (Gojira, *snippet*), drive gate + amp
gate (Mansoor, S11). No source gives numbers. → Recommendation in §3.3.

**F5. Mark-style graphic EQ.** All Mark-series amps use the same 5 bands: **80, 240, 750, 2200, 6600 Hz** (S31). The
band Q is "fairly wide around the centre points" (retail copy, S32). A forum suggestion (not an artist setting) for the
Petrucci-style sound: boosted mid-gain, low pre-EQ bass, mids around 10 o'clock, highs almost full, then a **V on the
post GEQ**, going easy on the top band (S33).

**F6. Presence/resonance.** The JP-2C has two presence frequencies (pull switch). Pushed in is the lower frequency,
"still above the treble", for "increased cut and focus" (S22). Resonance/depth corner frequencies are *unverified*.
→ Expose a power-amp "depth" (low shelf/peak 60–150 Hz) and "presence" (high shelf/peak 2–6 kHz, 2 voicings) on each
high-gain amp model.

**F7. Post-cab EQ for high-gain rhythm** (one source: Nail The Mix, S34, S35):

| Move | Range | Notes |
|---|---|---|
| HPF | 80–120 Hz (steep). Pre-distortion DI: 80–120 Hz. Second pair of quad-tracked guitars: 120–150 Hz | Gentler on 7/8-strings |
| Mud cut | 200–500 Hz, narrow | |
| Grind / honk | 700 Hz–1.5 kHz. Whistle/honk at 800 Hz–2 kHz, narrow cut | A boost here = "aggression" |
| Pick attack | 1.5–4 kHz. Rhythm boost +2–4 dB around 2–3 kHz, medium Q | |
| Harsh resonance | Sweep 3–5 kHz with a narrow boost, then cut | |
| Presence | 5–8 kHz | |
| Fizz | ≥ 7 kHz. LPF 10–12 kHz or high shelf from 8–10 kHz | A broad LPF can dull the sound, so prefer notches |
| Pre-distortion mid boost | +3–6 dB, medium Q, 700 Hz–1.2 kHz + HPF ~100 Hz | Equals the TS role |

## 3. Recommendations

### 3.1 DSP blocks this lineup needs (in addition to tone-targets-a)

| Block | Parameters / ranges | Algorithm / note | License |
|---|---|---|---|
| Noise gate (input, global) | thr −90…−20 dBFS, hyst 0–10 dB, attack 0.1–10 ms, hold 0–200 ms, release 10–500 ms, range −90…0 dB | RMS/peak detector on the **pre-boost** signal, gain applied with a smoothed (one-pole) envelope. Optional 2nd instance post-amp, keyed from the input (Decimator-style) | Own code |
| TS-style boost | drive 0–1, tone 0–1, level −20…+12 dB, "bright" sw | §F1 topology. 2× oversampling is enough at low drive (*unverified*: check aliasing per #10) | Own code |
| Tight HPF ("attack") | 6 steps or continuous 20–300 Hz, 12/24 dB/oct | Biquad (TPT SVF) | Own code |
| Graphic EQ voicings | 9-band (ISO octaves 63 Hz–16 kHz) and 5-band Mark-style (80/240/750/2200/6600) ±12 dB, Q ≈ 0.7–1.0 (*unverified*) | Constant-Q peaking biquads | Own code |
| Semi-parametric 4-band + HPF/LPF | HPF 20–500 Hz, LPF 2–20 kHz, bands ±15 dB, Q 0.3–6 | RBJ biquads | Own code |
| Power-amp depth / presence | depth 60–150 Hz ±10 dB, presence 2–6 kHz ±10 dB, 2 presence voicings | Shelves inside the amp model's NFB loop when using a white-box amp. Static EQ around NAM otherwise | Own code |
| Polyphonic octave-down | −1, −2 oct, dry/oct levels | Filter-bank + frequency division or PSOLA. Decide in #11 | Own code / TBD |
| Whammy pitch | ±24 st, glide time, detune ±0–25 cents | Granular/PSOLA, latency < 10 ms target | TBD (#11) |
| Spring reverb + tremolo | decay 1–3 s, trem rate 1–10 Hz, depth 0–100 % | Dispersive allpass chain. Bias-style (opto) trem | Own code |
| Doubler | delay 10–30 ms, ±10 cents mod, width | Two modulated delay lines + pitch | Own code |

### 3.2 Target preset types (original names chosen later by the preset designer)

Spectral notes are *unverified* starting hypotheses. The preset designer turns them into measured long-term
spectra (63 Hz–8 kHz octave bands) once renders exist.

| Inspiration ("aims at a tone similar to") | Preset types (3–5) | Key settings / spectral shape |
|---|---|---|
| Gojira | (1) Ultra-tight drop-tuned rhythm. (2) Pick-scrape / whammy-dive lead. (3) Clean arpeggio with tape delay + dark shimmer. (4) Crunch with phaser | 5150-style rhythm channel, TS boost (drive 0, level max), HPF ~100 Hz, gate thr ≈ −50 dBFS fast release. Dip ~400 Hz, lift 1–2 kHz |
| Nolly | (1) Modern "djent" palm-mute rhythm (bright OD → high-gain). (2) Crunch with a British twist. (3) Full rhythm for 7/8-strings. (4) Clean with comp + room | OD bright on. Gain set so palm-mutes stay coherent (§2.2, Nolly row). HPF 80–100 Hz (gentler for ERG), mod-EQ narrow cut 3–5 kHz |
| Petrucci | (1) Mark-style scooped rhythm (GEQ V). (2) Singing lead (lead ch. + delay + plate). (3) Chorus-clean with stereo delay. (4) Piezo-acoustic. (5) Wah lead | 5-band GEQ: 80 +2, 240 −1, 750 −3, 2200 +1, 6600 +2 dB (*unverified* V). Comp before the clean |
| Misha Mansoor | (1) Tight 6/8-string rhythm (TS-style, drive 0, attack sw high). (2) Swedish-"chainsaw"-style distortion. (3) Glitch/laser ambient. (4) Shimmer clean | Attack-HPF 100–130 Hz. Pre-and-post gates. Chainsaw: big 1 kHz + 100 Hz boosts (*unverified*) |
| Tom Morello | (1) Cranked 800-style with EQ-pedal boost. (2) Whammy +5th harmony. (3) Kill-switch / scratch wah. (4) Slapback + long-delay lead | Bass/mid 10, treble/presence 7, gain 9. Flat EQ boost +6 dB. Delays 12.5–800 ms |
| John Mayer | (1) Blackface 1×15 clean with spring. (2) Boutique "singing" edge with Klon-style. (3) TS-10 blues lead. (4) Slapback trem clean. (5) 3-amp blend | Always-on clean boost (+3…+6 dB). Slapback ~100 ms. Watch the low end (users find it bass-heavy): HPF 70–90 Hz |
| Mateus Asato | (1) British edge-of-breakup (tight, bright). (2) Glassy modern clean + opto comp. (3) Tape-delay lead. (4) Chorus/vibrato clean | Opto comp 3–6 dB GR. Tape delay 300–450 ms, 25–35 % fb, wow/flutter (*unverified*) |

### 3.3 Gate calibration procedure (QA, Phase 3+)

1. Measure the noise floor of each DI clip in `assets/di/` in silent sections (RMS, dBFS) and its peak level.
2. Default threshold = noise floor + 10 dB, clamped to −70…−40 dBFS. Hysteresis 4 dB.
3. Test: on a palm-muted 16th-note DI at 160 BPM, the gate must open on every note (no dropped transient > 1 ms
   late) and close within 100 ms after each stop. Measure through the full high-gain preset.

## 4. Open questions (for the architect)

1. Ship **one global input gate** (simpler) or **two** (input + post-amp keyed from the input)? Recommendation: one
   global gate with an optional "post" tap, decided in the signal-flow spec.
2. TS-style boost: white-box circuit model (WDF) or the cheaper EQ→clipper→EQ approximation? It affects CPU use and
   the "zero drive" character (F2).
3. Polyphonic octave and whammy pitch: in scope for v1? They are needed for two inspirations (Gojira, Morello) but
   carry latency and CPU risk. Coordinate with #11.
4. Do high-gain amp channels use NAM captures (static EQ around them) or white-box models (depth/presence inside the
   loop)? This affects F6.
5. Should the 9-band and 5-band GEQs be one block with "voicings", or two blocks?

## 5. Sources (all accessed 2026-10-25)

| ID | URL |
|---|---|
| S1 | https://neuraldsp.com/plugins/archetype-gojira |
| S2 | https://www.soundonsound.com/news/neural-dsp-release-archetype-gojira-x |
| S3 | https://neuraldsp.com/plugins/archetype-nolly |
| S4 | https://www.nailthemix.com/best-neural-dsp-plugins |
| S5 | https://neuraldsp.com/plugins/archetype-petrucci |
| S6 | https://www.soundonsound.com/reviews/neural-dsp-archetype-misha-mansoor-x |
| S7 | https://www.soundonsound.com/reviews/neural-dsp-archetype-tom-morello |
| S8 | https://neuraldsp.com/articles/tom-morello-pedalboard-and-amp-settings |
| S9 | https://neuraldsp.com/news/introducing-archetype-john-mayer-x |
| S10 | https://guitar.com/reviews/effects-pedal/hands-on-neural-dsp-archetype-john-mayer-review |
| S11 | https://www.sweetwater.com/insync/how-to-dial-in-a-modern-metal-tone-with-misha-mansoor/ |
| S12 | https://mixdownmag.com.au/news/neural-dsp-launch-archetype-mateus-asato/ |
| S13 | https://www.bonedo.de/artikel/einzelansicht/neural-dsp-archetype-gojira-test.html ; https://unity.neuraldsp.com/t/guitar-noise/8116 |
| S14 | https://reverb.com/news/inside-gojiras-touring-rig (fetch blocked, 403. Search snippet only) |
| S15 | https://musicradar.com/news/gojira-joe-duplantier-interview (body truncated. Search snippet only) |
| S16 | https://www.tone3000.com/tones/gojira-joe-duplantier-25477 (community capture, secondary) |
| S17 | https://www.riffhard.com/how-to-get-a-gojira-guitar-tone/ |
| S18 | https://musictech.com/news/gear/djent-guitar-tone-neural-dsp-plug-in-adam-nolly-getgood-periphery/ |
| S19 | https://www.guitarworld.com/tag/tone (teaser of the "busts myths about high-gain tone" interview. Snippet only) |
| S20 | https://www.premierguitar.com/gear/rig-rundown-dream-theaters-john-petrucci |
| S21 | https://www.rig-talk.com/forum/threads/petruccis-new-touring-rig-for-2016.175765/ (forum, secondary) |
| S22 | https://www.zzounds.com/item--MESJP2CHEAD (retail copy of the manufacturer's JP-2C description) |
| S23 | https://equipboard.com/items/neural-dsp-archetype-john-mayer-x (aggregated user reports) |
| S24 | https://www.ecircuitcenter.com/Circuits/distortion_box/op_disto1.htm |
| S25 | https://diystompboxes.com/smfforum/index.php?msg=617977 |
| S26 | https://reverb.com/news/using-pedals-to-shape-high-gain-amp-tones (search snippet) |
| S27 | https://violetrecording.com/what-is-a-tube-screamer/ |
| S28 | https://line6.com/support/topic/31999-using-a-tubescreamer (forum) |
| S29 | https://forum.kemper-amps.com/forum/thread/11826-till-s-chimera-pack-till-schleicher/?pageNo=6 (forum) |
| S30 | https://en.wikipedia.org/wiki/Noise_gate |
| S31 | https://www.premierguitar.com/articles/MesaBoogie_Mark_V_Amp_Review |
| S32 | https://reverb.com/item/102459229-mint-mesa-boogie-mark-iic-amplifier-head-75w (retail listing) |
| S33 | https://forum.fractalaudio.com/threads/mesa-boogie-jp-2c-tone.218976/ (forum, not an artist setting) |
| S34 | https://nailthemix.com/low-mean-eqing-modern-metal-guitars-for-max-impact |
| S35 | https://nailthemix.com/low-pass-filter |
| S36 | https://neuraldsp.com/pages/downloads (lineup and versions: Gojira X, Nolly X, Petrucci X, Misha Mansoor X, John Mayer X, Tom Morello, Mateus Asato) |
