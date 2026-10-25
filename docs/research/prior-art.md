# Open-source prior art (licence-compatible)

- Issue: #12
- Author: acct1/researcher (Sable)
- Date: 2026-10-25 (all sources accessed on that date unless stated otherwise)
- Licence data: GitHub repository metadata (`gh api repos/<owner>/<repo>`), LICENSE files and source headers, read
  on 2026-10-25. "Activity" means the last push date.

## 1. Summary

1. **Our licence (AGPL-3.0-or-later) can absorb** MIT, BSD, ISC, Apache-2.0, zlib, LGPL-2.1/3.0, GPL-2.0-or-later,
   GPL-3.0 (only or or-later) and AGPL-3.0 code. GPLv3 and AGPLv3 may be combined (§13 of each), and GPL-2.0-or-later
   code can be taken under GPLv3 [S-FSF].
2. **GPLv2-only (incompatible): rakarrack-plus.** Its headers say "the terms of version 2 of the GNU General Public
   License", with no "or later" clause [S16]. **Study it only. Never copy code from it.** No other surveyed project is
   GPLv2-only.
3. **Watch-outs that are compatible but have side effects:**
   - GPL-3.0-**only** code, and the two `AGPL-3.0-only` functions in Faust libraries, would pin the combined work to
     v3 (no "or later" for those parts).
   - chowdsp_utils licenses each module separately (BSD core, GPLv3 DSP modules).
   - Guitarix's bundled IRs and fonts carry separate licence files.
4. **Vendor candidates (permissive, actively maintained, and useful for `dsp-core`):** NAM core (MIT), RTNeural
   (BSD-3), NeuralAudio (MIT), Airwindows (MIT; algorithm ideas per plugin), Faust `aanl.lib` (MIT, anti-aliased
   nonlinearities) and Faust STK-4.3/MIT functions translated into C++, and the CloudSeed core (MIT).
5. **Study, and copy only with attribution under the GPL:** BYOD and ChowTape (GPLv3, circuit-modelled drive and tape),
   Guitarix (GPL-2.0+, tube-stage models), Surge XT FX (GPLv3), Dragonfly Reverb (GPLv3, Freeverb3-based), AIDA-X /
   Proteus / NeuralPi (GPLv3, RTNeural LSTM players), and KPP (GPLv3+).
6. **Reference integrations for our amp block:** NeuralAmpModelerPlugin (MIT), nam-juce (GPLv3, JUCE-based, closest
   to our stack), Ratatouille (BSD-3, NAM + RTNeural mixer with IR loader), and ToobAmp/PiPedal (MIT, LV2 amp/cab
   stack).
7. **Reverb without a proprietary design:** use the published literature (Schroeder/Moorer, Dattorro plate, Jot FDN)
   and the public-domain Freeverb topology [S24–S26]. Commercial reverb vendors' blog posts may be read for ideas.
   Their products are never inspected.
8. **Each copied file keeps its original header** and gets a `THIRD_PARTY.md` entry (AGENTS.md rule 9).

## 2. Findings

### 2.1 Amp, neural and pedalboard projects

| Project | Licence (exact) | AGPLv3-compatible | Activity | Learn / reuse | Source |
|---|---|---|---|---|---|
| NeuralAmpModelerCore | MIT | Yes | 2026-10-01 | `.nam` runtime (see `nam-runtime.md`) | S1 |
| NeuralAmpModelerPlugin | MIT | Yes | 2026-08-23 | Model staging, calibration, resampling wrapper (do not copy its unsynchronised swap) | S2 |
| neural-amp-modeler (trainer) | MIT | Yes | 2026-08-23 | Training our own captures | S3 |
| RTNeural | BSD-3-Clause | Yes | 2026-08-20 | LSTM/GRU/Conv1D inference, compile-time models | S4 |
| NeuralAudio | MIT | Yes | 2026-10-09 | Optimised NAM/LSTM runtime | S5 |
| nam-juce | GPL-3.0 | Yes | 2026-08-23 | JUCE wrapper for NAM (our framework) | S6 |
| neural-amp-modeler-lv2 | GPL-3.0 | Yes | 2026-09-04 | RT-safe model loading in LV2 | S7 |
| AIDA-X | GPL-3.0-or-later (README) | Yes | 2024-12-13 | RTNeural LSTM player + cab IR, DPF-based | S8 |
| Proteus | GPL-3.0 (badge; LICENSE.txt) | Yes | 2023-07-08 (inactive) | LSTM capture player (JUCE) | S9 |
| NeuralPi | GPL-3.0 | Yes | 2024-01-18 | Embedded neural amp + IR | S10 |
| SmartGuitarAmp | Apache-2.0 | Yes | 2023-04-11 (inactive) | Early WaveNet amp plugin | S11 |
| Chameleon | GPL-3.0 | Yes | 2023-01-05 (inactive) | LSTM amp with knob conditioning | S12 |
| Ratatouille.lv2 | BSD-3-Clause | Yes | 2026-06-22 | Blend of two NAM/RTNeural models + 2 IRs, buffer handling | S13 |
| NeuralRack | BSD-3-Clause | Yes | 2026-10-10 | Model + IR loader (uses NeuralAudio) | S14 |
| Guitarix | GPL-2.0-or-later (source header) | Yes (as GPLv3+) | 2026-10-05 | Faust-generated tube stages, tone stacks, huge FX set. IRs and fonts have their own licences. | S15 |
| rakarrack-plus | **GPL-2.0-only** (source header) | **No** | 2026-05-10 | Study only | S16 |
| ZynAddSubFX (effects) | GPL-2.0-or-later (source header) | Yes (as GPLv3+) | 2026-09-10 | Classic chorus/phaser/distortion code | S17 |
| Kapitonov Plugins Pack | GPLv3+ (README) | Yes | 2025-12-22 | Tube-amp LV2 with profile loader | S18 |
| ToobAmp | MIT (LICENSE.md) | Yes | 2026-07-27 | LV2 amp/cab/tone stack, NAM + ML players | S19 |
| PiPedal | MIT for original content (LICENSE.md; bundled deps differ) | Yes | 2026-10-12 | Pedalboard host UX, preset model | S20 |
| stompbox | GPL-3.0 | Yes | 2026-09-29 | Pedalboard engine using NeuralAudio | S21 |
| TONE3000 plugin | MIT | Yes | 2026-10-06 | NAM player (A2 support) | S22 |

### 2.2 DSP libraries and effect collections

| Project | Licence (exact) | AGPLv3-compatible | Activity | Learn / reuse | Source |
|---|---|---|---|---|---|
| chowdsp_utils | Per module: BSD (`chowdsp_core`, data structures, …); **GPLv3** (`chowdsp_dsp_utils`, `chowdsp_compressor`, `chowdsp_dsp_data_structures`, …); non-module code GPLv3 | Yes (all of them) | 2026-10-22 | Filters, WDF, SIMD helpers. JUCE-module form, so only for `app/`, or port to `dsp-core`. | S23 |
| BYOD | GPL-3.0 (dual-licensed per `docs/Licensing.md`; second licence *unverified*) | Yes | 2025-08-14 | Drive circuits (WDF, ADAA), modular board | S27 |
| ChowTape (AnalogTapeModel) | GPL-3.0 (iOS build also MPL-2.0) | Yes | 2025-04-09 | Hysteresis tape model, oversampling choices | S28 |
| Surge XT (FX section) | GPL-3.0 | Yes | 2026-10-25 | Mature chorus/flanger/delay/reverb/waveshapers with SIMD | S29 |
| Airwindows | MIT | Yes | 2026-10-24 | Hundreds of small, documented algorithms (saturation, console, reverb) | S30 |
| Faust libraries | Per function: 238× `LicenseRef-STK-4.3` (permissive), 144× MIT, 35× LGPL-2.1-or-later-with-Faust-exception, 27× GPL-3.0-only, 5× LGPL-2.1-only, 2× LGPL-3.0-only, 2× GPL-2.0-or-later, **2× AGPL-3.0-only**, 1× ISC, 1× BSD-3 | Yes (all of them; the AGPL-3.0-only and GPL-3.0-only functions pin v3) | 2026-10-24 | `aanl.lib` (all MIT: ADAA nonlinearities), `vaeffects.lib`, `wdmodels.lib`, `filters.lib`, `compressors.lib` | S31 |
| Dragonfly Reverb | GPL-3.0 (bundles Freeverb3, GPL; version *unverified*) | Yes | 2026-05-21 | Hall/room/plate/early-reflection algorithms | S32, S33 |
| CloudSeed | MIT | Yes | 2024-05-26 | Modulated delay-network reverb core (C++) | S34 |
| CloudReverb | GPL-3.0 | Yes | 2026-09-05 | CloudSeed algorithm port | S35 |
| LSP Plugins | LGPL-3.0 | Yes | 2026-10-21 | Compressors, gates, EQ, analyzers | S36 |
| Calf Studio Gear | LGPL-2.1-or-later (source header) | Yes | 2026-10-13 | Classic LV2 FX (reverb, mod, dynamics) | S37 |
| darc.lv2 (x42) | GPL-3.0 | Yes | 2026-09-07 | Dynamic-range compressor design | S38 |

### 2.3 Published algorithms (no code licence involved)

| Topic | Reference | Use | Source |
|---|---|---|---|
| Plate reverb | J. Dattorro, "Effect Design Part 1", JAES 1997 | Plate topology with modulated allpasses | S24 |
| Freeverb | Jezar's public-domain Schroeder/Moorer design (8 combs + 4 allpasses) | Baseline room reverb | S25 |
| FDN reverb | Jot & Chaigne FDN, explained in J. O. Smith, *Physical Audio Signal Processing* | Hall/room with a stable, tunable decay | S26 |
| Commercial-reverb design blogs | Read for ideas only. Never inspect products. | Reading list for the reverb ticket | *unverified (site returned 403)* |

## 3. Recommendations

| Category | Projects | Verdict |
|---|---|---|
| **Vendor or fetch as a dependency** | NAM core, RTNeural, NeuralAudio, Airwindows (selected algorithms), CloudSeed core, Faust `aanl.lib` functions (ported), HIIR (see `oversampling.md`) | Permissive. Keep headers and add `THIRD_PARTY.md` entries. |
| **May copy code, with GPL attribution** | Guitarix, BYOD, ChowTape, Surge XT FX, Dragonfly, LSP, Calf, ZynAddSubFX FX, KPP, AIDA-X, nam-juce | Compatible. Keep the original copyright headers. Prefer re-implementing from the paper or idea so that `dsp-core` stays easy to relicense or embed (open question 1). |
| **Study only** | rakarrack-plus (GPL-2.0-only), any file whose licence we cannot state | Never copy. |
| **Faust** | Use Faust `.lib` functions as readable references for C++ ports. Do not add the Faust compiler to the build in v1. Check each function's `declare license`. | Avoid the two `AGPL-3.0-only` and the GPL-3.0-only functions unless needed. |
| **Assets inside these projects** | Guitarix IRs and fonts, any bundled captures | Separate licences. Each file needs an `assets/LICENSES.md` row before use. Default: do not bundle. |

Process rule for code reuse: a PR that copies third-party code names the source repo, commit SHA, file path and
licence in the PR body and in `THIRD_PARTY.md`. The reviewer checks the header was kept.

## 4. Open questions (for the architect)

1. **Should `dsp-core` stay permissive-only (MIT/BSD/zlib/STK/CC0)** so it could later be dual-licensed or embedded in
   hardware? Or may it take GPLv3 code (it is AGPL either way today)? This decides whether BYOD/Guitarix code may be
   copied or only re-implemented.
2. Should we add Faust as an offline code generator (its generated code takes the licences of the libraries used), or
   port selected functions by hand?
3. Is chowdsp_utils (a JUCE module) worth using in `app/` and `ui/` for presets/parameters, or do we keep our own?
4. Should the amp section copy the BYOD/Guitarix approach (WDF circuit models + ADAA) for the non-NAM "classic" amps,
   or use simpler static waveshapers + tone stacks?

## 5. Sources (all accessed 2026-10-25)

- S-FSF: FSF, "Various Licenses and Comments about Them" (GPL compatibility list). https://www.gnu.org/licenses/license-list.en.html (*HTTP 429 today; from prior knowledge*)
- S1: https://github.com/sdatkinson/NeuralAmpModelerCore
- S2: https://github.com/sdatkinson/NeuralAmpModelerPlugin
- S3: https://github.com/sdatkinson/neural-amp-modeler
- S4: https://github.com/jatinchowdhury18/RTNeural
- S5: https://github.com/mikeoliphant/NeuralAudio
- S6: https://github.com/Tr3m/nam-juce
- S7: https://github.com/mikeoliphant/neural-amp-modeler-lv2
- S8: https://github.com/AidaDSP/AIDA-X (README "License")
- S9: https://github.com/GuitarML/Proteus
- S10: https://github.com/GuitarML/NeuralPi
- S11: https://github.com/GuitarML/SmartGuitarAmp
- S12: https://github.com/GuitarML/Chameleon
- S13: https://github.com/brummer10/Ratatouille.lv2
- S14: https://github.com/brummer10/NeuralRack
- S15: https://github.com/brummer10/guitarix (`trunk/COPYING`, header of `trunk/src/gx_head/engine/gx_engine.cpp`)
- S16: https://github.com/Stazed/rakarrack-plus (header of `src/EFX_common/AnalogFilter.C`)
- S17: https://github.com/zynaddsubfx/zynaddsubfx (header of `src/Effects/Alienwah.cpp`)
- S18: https://github.com/olegkapitonov/Kapitonov-Plugins-Pack (README "License")
- S19: https://github.com/rerdavies/ToobAmp (`LICENSE.md`)
- S20: https://github.com/rerdavies/pipedal (`LICENSE.md`)
- S21: https://github.com/mikeoliphant/stompbox (`LICENSE.md`)
- S22: https://github.com/tone-3000/tone3000-plugin
- S23: https://github.com/Chowdhury-DSP/chowdsp_utils (`LICENSE.md`, README module list)
- S24: J. Dattorro, "Effect Design Part 1: Reverberator and Other Filters", JAES 45(9), 1997. https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
- S25: J. O. Smith, "Freeverb", *Physical Audio Signal Processing*. https://ccrma.stanford.edu/~jos/pasp/Freeverb.html
- S26: J. O. Smith, "FDN Reverberation", *Physical Audio Signal Processing*. https://ccrma.stanford.edu/~jos/pasp/FDN_Reverberation.html
- S27: https://github.com/Chowdhury-DSP/BYOD (README "License")
- S28: https://github.com/jatinchowdhury18/AnalogTapeModel (README "License")
- S29: https://github.com/surge-synthesizer/surge (`LICENSE`)
- S30: https://github.com/airwindows/airwindows ; https://www.airwindows.com/
- S31: https://github.com/grame-cncm/faustlibraries (`declare license` tags counted across all `*.lib`; `licenses/stk-4.3.0.md`)
- S32: https://github.com/michaelwillis/dragonfly-reverb (`LICENSE`, README)
- S33: Freeverb3 project page. https://www.nongnu.org/freeverb3/
- S34: https://github.com/ValdemarOrn/CloudSeed
- S35: https://github.com/xunil-cloud/CloudReverb
- S36: https://github.com/lsp-plugins/lsp-plugins
- S37: https://github.com/calf-studio-gear/calf (header of `src/modules_dist.cpp`)
- S38: https://github.com/x42/darc.lv2
