# Redistributable assets: .nam captures, cab IRs and DI test clips

- **Issue:** #7 · **Author:** acct3/researcher (Wren) · **Date:** 2026-10-25
- **Question:** which captures, IRs and DI clips can we ship inside an AGPL-3.0-or-later app? Can we generate our own?
- **Method:** licence pages and repository files only, every one read on 2026-10-25. **A licence counts only when the
  text is quoted below.** "Free download" with no stated terms is treated as **not redistributable**. Nothing was taken
  from a commercial plugin.
- **Internal note.** The brand and product names below stay in `docs/research/`. Shipped assets get neutral names
  (rule 2). The new names are in `tools/guard/forbidden_names.txt`.

## 1. Summary

1. **Bundle-ready now (Y), all with exact terms quoted in §2:**
   - **IRs:** 6 CC0 4×12 IRs (Freesound pack 34071, 48 kHz / 24-bit) and 1 CC0 1×12 IR (Freesound 507167, 96 kHz).
     The 18 short built-in guitarix cabinet IRs (GPL-2.0-or-later) are a fallback.
   - **DI:** 1 CC0 strum/arpeggio DI clip (Freesound 453384, 10.75 s). EGFxSet "Clean" single notes (CC BY 4.0).
     Synthetic signals we generate ourselves.
   - **NAM:** the MIT example models in NeuralAmpModelerCore, **for tests only**: they carry no metadata on what was
     captured.
2. **Rejected (N):**
   - the TONE3000 library: its terms forbid redistribution without written permission from both the creator and the site;
   - IDMT-SMT-Guitar (CC BY-NC-ND 4.0, non-free);
   - Voxengo reverb IRs (a "no profit from distribution" clause, so non-free);
   - the guitarix "BestPlugins" IR folders (GPL, but named after trademarked amps and album tones, provenance unclear);
   - every "free" cab pack whose page states no redistribution terms.
3. **There is no redistributable high-quality amp capture.** Recommendation: **make our own `.nam` captures** by
   training the MIT NAM trainer on renders of our own white-box amp models or SPICE simulations of generic circuits.
   The project then owns the output and can license it under AGPL-3.0-or-later or CC0.
4. **Generate IRs ourselves** as the long-term path. Design a minimum-phase FIR from a parametric "speaker + mic"
   magnitude target, which needs no hardware. Then measure a real cab with an exponential sine sweep if hardware ever
   becomes available (§2.4).
5. **CC0 and CC BY 4.0 are both GPL-compatible for non-code works** (FSF, S1). CC BY needs the attribution block in
   `assets/LICENSES.md` and an "About / Credits" screen. NC and ND licences are excluded.
6. Every asset PR must copy the exact licence line and the source URL into `assets/LICENSES.md`, rename the file
   neutrally, and add a SHA-256 hash.

## 2. Findings

### 2.1 Licence compatibility (FSF licence list, S1)

| Licence | FSF verdict (quoted) | Our verdict |
|---|---|---|
| CC0 1.0 | "Both public domain works and the lax license provided by CC0 are compatible with the GNU GPL." | **Y** |
| CC BY 4.0 | "a non-copyleft free license that is good for art and entertainment works … compatible with all versions of the GNU GPL" | **Y**, with attribution |
| CC BY-NC (any) | "does not qualify as free, because there are restrictions on charging money for copies" | **N** (AGPL allows selling copies) |
| CC BY-ND 4.0 | listed as a licence for "works of opinion" (verbatim copying only) | **N** for assets (no derivatives: we resample and trim IRs) |
| GPL-2.0-or-later | upgradable to GPL-3, which is compatible with AGPL-3 (AGPL §13 / GPL-3 §13) | **Y** |
| MIT | permissive | **Y** |

### 2.2 Asset table

"Redistribute?" means: may we ship it inside OpenSourcePedal installers and the repo? **Rec** is our verdict.

| # | Asset | URL | Licence (exact text from the page) | Redistribute? | Attribution text | Rec |
|---|---|---|---|---|---|---|
| I1 | 4×12 cab IRs, 6 files (V30/DV77 speakers, SM57 / PDMIC mics), 48 kHz, 24-bit, 1.196–1.275 s, uploaded 2022 | https://freesound.org/people/jesterdyne/packs/34071/ (sounds 616702–616705, 618896, 618897) | "Creative Commons 0": "You can copy, modify, distribute and perform the sound, even for commercial purposes" (each sound page checked) | Yes | Not required. Courtesy: "Cab IRs by jesterdyne (freesound.org), CC0" | **Y** |
| I2 | 1×12 "Brown Cab" IR, Audix i5 mic, 96 kHz, 24-bit, 0.5 s, 2020 | https://freesound.org/people/tosha73/sounds/507167/ | "Creative Commons 0" (same wording) | Yes | Courtesy: "IR by tosha73 (freesound.org), CC0" | **Y** |
| I3 | guitarix built-in cab IRs, 18 arrays (68–1000 taps at 48 kHz, i.e. 1.4–21 ms) | https://github.com/brummer10/guitarix/blob/master/trunk/src/LV2/DSP/cab_data.cc | File header: "either version 2 of the License, or (at your option) any later version" (GNU GPL). © 2012 Hermann Meyer, Andreas Degert, Pete Shorthose, Steve Poskitt | Yes (GPL-2.0+ → used under GPL-3) | "Cabinet IR data © 2012 H. Meyer, A. Degert, P. Shorthose, S. Poskitt (guitarix), GPL-2.0-or-later" | **Y (fallback)**. Rename: the arrays are named after amps. Who measured them is *unverified*. Very short, so weak below ~150 Hz |
| I4 | guitarix `IR/BestPlugins_*` (amp- and album-named IRs) | https://github.com/brummer10/guitarix/tree/master/trunk/IR | LICENSE: "Copyright (C) 2013 David Fau Casquel … GNU General Public License … version 2 of the License, or (at your option) any later version" | Legally yes | n/a | **N**: file names are trademarks and band/album titles, and the "tone match" provenance (possibly EQ-matched from commercial records) is *unverified* |
| I5 | Freesound pack 6385 "Speaker Impulse Responses" (45 files) | https://freesound.org/people/jesterdyne/packs/6385/ | Per sound. The uploader has "Attribution (149)" and "Creative Commons 0 (24)" sounds. The one checked (129399) is CC0 but "Impulse Response of the Cabinet Simulation on the AX1500g Unit", 5 ms, 44.1 kHz/16-bit | Mixed | n/a | **N**: captures of a commercial modeller's cab sim, too short, mixed licences |
| I6 | Voxengo free reverb IRs | https://www.voxengo.com/impulses/ | "use these impulse files royalty-free for any purpose, including commercial usage" **but** "You may not sell these impulse files or earn any direct or indirect profit from their distribution" | Restricted | n/a | **N**: non-free clause conflicts with AGPL resale freedom |
| I7 | OpenAIR room/hall IRs (for reverb, not cab) | https://www.openair.hosted.york.ac.uk/?page_id=2 | Per item: "Any one of the Creative Commons licenses can be chosen, or the user can reserve all rights" (search summary of the site, S11) | Per item | CC BY items: "<title> by <creator>, OpenAIR, CC BY 4.0" | **Conditional**: only items whose page says CC0 / CC BY |
| I8 | Free "metal cab" / boutique free IR packs (Kalthallen, Erik Ekholm "Metal Cabs", Devil's Lab, Origin Effects, ML Sound Lab freebies, God's Cab) | (S12–S14) | No redistribution terms found, or site unreachable (Kalthallen: connection refused) | **Unknown → No** | n/a | **N** until written permission is on file |
| N1 | NeuralAmpModelerCore `example_models/*.nam` (9 files, e.g. `A2.nam` 0.7.0 SlimmableContainer, 48 kHz) | https://github.com/sdatkinson/NeuralAmpModelerCore/tree/main/example_models | Repo LICENSE: "MIT License / Copyright (c) 2023 Steven Atkinson" | Yes | "NAM example models © Steven Atkinson, MIT" | **Y, tests only**. The metadata names no device. `my_model.nam` has no metadata at all |
| N2 | TONE3000 library (formerly ToneHunt) | https://www.tone3000.com/terms | "You may not package, bundle, or redistribute tones obtained from TONE3000 through third-party platforms" and "You may not sell, license, or commercially distribute content … without explicit written permission from both the content owner and TONE3000" | No | n/a | **N** |
| N3 | AIDA-X default model `tw40_california_clean_deerinkstudios.json` + IR `V30-P2-audix-i5-deerinkstudios.wav` | https://github.com/AidaDSP/AIDA-X/tree/main/files | No file-specific licence. Repo: "AIDA-X is licensed under `GPL-3.0-or-later`" | *Unverified*: the repo licence may not have been granted for third-party studio files | n/a | **N for now** (open question 3). Also not NAM format |
| D1 | "Clean Strumming & Arpeggio", single-pickup solid-body DI, 44.1 kHz/16-bit, 10.75 s, 2018 | https://freesound.org/people/loudernoises/sounds/453384/ | "Creative Commons 0". Description: "Recorded for the purpose of testing out guitar DSP effects" | Yes | Courtesy: "DI clip by loudernoises (freesound.org), CC0" | **Y** (first golden-render clip, about 0.95 MB, under the LFS limit) |
| D2 | EGFxSet "Clean.zip" (431 MB): single notes on a 22-fret single-coil guitar, 5 pickup positions, 5 s each | https://zenodo.org/record/7044411 | "Creative Commons Attribution 4.0 International" | Yes | "EGFxSet, H. Pedroza et al. (2022), https://zenodo.org/record/7044411, CC BY 4.0. Excerpts trimmed/resampled" | **Y**: ship ≤ 20 selected notes via LFS, not the whole set |
| D3 | GuitarSet 1.1.0 (acoustic, hexaphonic + mic) | https://zenodo.org/records/3371780 | "Creative Commons Attribution 4.0 International" | Yes | "GuitarSet, Q. Xi et al. (2019), https://zenodo.org/records/3371780, CC BY 4.0. Excerpts" | **Y, piezo/acoustic tests only** (not an electric DI) |
| D4 | IDMT-SMT-Guitar 1.0.0 | https://zenodo.org/records/7544110 | "Creative Commons Attribution Non Commercial No Derivatives 4.0 International" | No (NC) | n/a | **N** |
| D5 | EGDB (240 hexaphonic DI takes) | (S15) | No licence statement found | Unknown | n/a | **N** until a licence is found |
| D6 | NAM `loudness_input.wav` (1.0 s, 48 kHz, 24-bit) | https://github.com/sdatkinson/neural-amp-modeler/blob/main/nam/models/_resources/loudness_input.wav | Repo: "MIT License / Copyright (c) 2019-2025 Steven Atkinson" | Yes | "© Steven Atkinson, MIT" | **Y, tests only** (too short for demos) |
| S1 | Synthetic signals: ESS sweeps, impulses, multitone, steps, noise, Karplus-Strong "DI" | generated by `tools/` (to be written) | Ours: AGPL-3.0-or-later (code), outputs CC0 | Yes | n/a | **Y** (the backbone of unit tests) |

### 2.3 Capture strategy for `.nam`

- No redistributable library of quality amp captures was found (N2, N3). Captures are "licensed separately by whoever
  captured them" (S16).
- The NAM trainer and core are MIT (S6, S7). Training on **signals we own** gives a model we own. Two routes:
  1. **White-box → NAM.** Render our own amp models (dsp-core) with the standard NAM training input, then train. Use:
     cheap runtime for heavy white-box models, A/B regression targets.
  2. **SPICE → NAM.** Simulate generic, public circuit topologies (12AX7 gain stages, TS-style clipper, generic
     tone stacks) in ngspice, then train. The models get neutral names. A capture of a trademarked product is legal to
     ship only if renamed (issue #7 text), but we avoid that class entirely.
- The NAM training input file distributed by the NAM project has no licence statement in the repository I checked.
  *Unverified*: QA must confirm its licence before committing it. Until then, generate our own excitation (sweeps +
  DI clips D1/D2).

### 2.4 Generating cab IRs ourselves

| Approach | How | Needs | Quality (expected) | Source |
|---|---|---|---|---|
| A. Parametric min-phase FIR | Design \|H(f)\| = HPF 2nd-order 70–110 Hz with +3–6 dB resonance, cone peaks 1.5–4 kHz, steep roll-off 4.5–7 kHz (24–48 dB/oct), optional comb from mic distance. Convert to minimum phase (homomorphic cepstrum: `scipy.signal.minimum_phase(…, method='homomorphic', half=False)` keeps the magnitude). 1024–4096 taps at 48 kHz | Only code (scipy BSD) | Plausible "generic cab", lacks fine resonances and mic/room detail (*unverified*, to be judged by spectral distance to I1/I2) | S8 |
| B. Measure a real cab | Exponential (log) sine sweep + deconvolution. Separates harmonic distortion from the linear IR | Cab, mic, interface, quiet room (owner hardware) | Best. Result is project-owned (CC0) | S9, S10 |
| C. Derive from CC0/GPL IRs | Mic-blend, EQ, window and resample I1–I3 into "voicings" | Code only | Good. Derivatives of CC0 are unrestricted, derivatives of GPL stay GPL | §2.1 |
| D. Physical speaker model | Thiele–Small LF + modal cone breakup + baffle | Research effort | Unknown (*unverified*), defer | n/a |

## 3. Recommendations

1. **First asset PR (≤ 10 MB, LFS for > 1 MB):**
   - I1 (6 IRs) and I2, renamed `cab/412-a.wav`…`cab/412-f.wav` and `cab/112-a.wav` (no speaker/brand codes in names), kept at native rate;
   - D1 as `di/strum-arp-01.wav`;
   - 10–20 EGFxSet notes (D2) as `di/notes/*.wav`;
   - one row each in `assets/LICENSES.md` with the exact licence line, URL, author, SHA-256, and "renamed from <original>".
2. **Keep I3** (guitarix arrays) as a compiled-in fallback cab only if the architect wants zero-file startup. Credit it
   in `THIRD_PARTY.md`. Otherwise skip it.
3. **NAM:** use N1 only in `tests/`. Open a ticket "own NAM captures from white-box/SPICE renders" (§2.3).
4. **IR tool:** add `tools/ir-design` (approach A, Python + scipy). It writes CC0 IRs with a JSON sidecar of its
   design parameters. Validate against I1/I2: octave-band distance ≤ 3 dB from 100 Hz to 5 kHz is the target
   (*unverified* threshold).
5. **Test signals:** `tools/testsignals` generates ESS (20 Hz–20 kHz, 10 s), unit impulse, −12/−18 dBFS multitone, and
   Karplus-Strong DI phrases at every rate in §4 of AGENTS.md. These are the golden-render inputs that need no licence.
6. **Policy for `assets/LICENSES.md`:**
   - accept CC0, CC BY 4.0, MIT, BSD, GPL-2.0+/GPL-3/AGPL-3;
   - reject NC, ND, "no redistribution", "no profit" and "free download, no terms";
   - attribution text appears in the app's Credits screen.

## 4. Open questions (for the architect)

1. Ship the guitarix short IRs (I3) as a compiled-in fallback, or require file IRs only?
2. Is approach A (designed IRs) good enough for factory presets, or only for tests? This decides whether the
   Phase-4 presets wait on more CC0 IRs.
3. AIDA-X files (N3): ask upstream for an explicit licence statement for the bundled studio files, or ignore them?
   (Contacting an outside project is outward-facing, so the orchestrator decides.)
4. Owner-recorded DI clips and cab measurements (approach B) would be the best project-owned assets. Raise it as a
   non-blocking wish in `docs/state/BLOCKERS.md`, or skip it?
5. CC BY attribution: is a Credits screen plus `assets/LICENSES.md` enough, or do we also want a text file installed
   next to the assets?

## 5. Sources (all accessed 2026-10-25)

| ID | URL |
|---|---|
| S1 | https://www.gnu.org/licenses/license-list.html (entries #CC0, #ccby, #CC-BY-NC, #ccbynd) |
| S2 | https://freesound.org/people/jesterdyne/packs/34071/ and sound pages 616702, 616703, 616704, 616705, 618896, 618897 |
| S3 | https://freesound.org/people/tosha73/sounds/507167/ |
| S4 | https://freesound.org/people/jesterdyne/packs/6385/ ; https://freesound.org/people/jesterdyne/sounds/129399/ |
| S5 | https://freesound.org/people/loudernoises/sounds/453384/ |
| S6 | https://github.com/sdatkinson/NeuralAmpModelerCore (LICENSE, example_models/, metadata read from the JSON) |
| S7 | https://github.com/sdatkinson/neural-amp-modeler (LICENSE, nam/models/_resources/loudness_input.wav) |
| S8 | https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.minimum_phase.html |
| S9 | https://aes2.org/publications/elibrary-page/?id=10211 (Farina, AES Conv. Paper 5093, 2000, ESS method. Abstract only) |
| S10 | https://angelofarina.it/Public/papers/226-AES122.pdf (Farina 2007 follow-up, not re-read in full) |
| S11 | https://www.openair.hosted.york.ac.uk/?page_id=2 (via search summary) |
| S12 | https://www.reasonstudios.com/blog/free-reverb-resources-where-to-find-impulse-responses (lists Kalthallen. Its page cabs.kalthallen.de refused connection) |
| S13 | https://bedroomproducersblog.com/2013/09/11/metal-cabs-free/ (no licence stated) |
| S14 | https://www.soundonsound.com/news/origin-effects-launch-free-ir-cab-library ; https://devilslabstudio.gumroad.com/l/devilslabfreepack (no licence stated) |
| S15 | https://astro.paperswithcode.com/dataset/egdb |
| S16 | https://docs.rs/crate/nam-rs/0.4.0/source/README.md (".nam model files are licensed separately by whoever captured them") |
| S17 | https://www.tone3000.com/terms ; https://tone3000.com/policy |
| S18 | https://github.com/brummer10/guitarix (trunk/COPYING, trunk/IR/*/LICENSE, trunk/src/LV2/DSP/cab_data.cc) |
| S19 | https://github.com/AidaDSP/AIDA-X (LICENSE, README "License", files/) |
| S20 | https://zenodo.org/record/7044411 (EGFxSet) ; https://zenodo.org/records/3371780 (GuitarSet) ; https://zenodo.org/records/7544110 (IDMT-SMT-Guitar) |
| S21 | https://www.voxengo.com/impulses/ |
