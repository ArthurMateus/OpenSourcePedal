# Framework comparison: JUCE 8 vs JUCE 9 vs iPlug2 (vs DPF and the CLAP-first options)

- Issue: #9. Author: acct3/researcher (Moth). Date: 2026-10-25.
- Scope: re-verify ADR-0001 ("AGPL-3.0-or-later; JUCE 8"). The central question is **which JUCE major to pin**.
  Also covered: iPlug2, DPF, and the CLAP-first options (clap-wrapper, nih-plug).
- Source tags such as [S1] refer to §5. All sources were accessed on 2026-10-25. Where a fact says "repo", I read
  it directly from the upstream Git tree or GitHub API on that date.

## 1. Summary (for the architect)

1. **Confirm JUCE as the framework, but pin JUCE 9, not JUCE 8.** The current release is **9.0.4**
   (2026-10-12) [S1]. The JUCE 8 line ended with **8.0.15** on 2026-07-21, the same day 9.0.0 shipped. Since then 8.x
   has had only a one-line **8.0.16-security** patch (2026-10-13) [S1].
2. **The open-source terms are unchanged.** JUCE 9's `LICENSE.md` keeps the modules "dual-licensed under the AGPLv3
   and the commercial JUCE licence". The only changes from 8.0.15 are: the EULA link (juce-8 → juce-9), a
   "no legal advice" paragraph, and the dependency list moving to an SPDX SBOM (`JUCE.spdx.json`) [S2]. The
   announcement says "Pricing and licensing terms have been kept identical to JUCE 8" [S3].
3. **The JUCE 9 EULA only binds us if we don't use the AGPL option.** We do use it, so the EULA's tiers, audit and
   publicity clauses don't apply [S2][S4]. A commercial licence for one major version does not cover the next
   (EULA clause 1.15) [S4]. That matters only to downstream commercial users, not to us.
4. **No native CLAP in JUCE 9.0.4.** The `FORMATS` list is `Standalone Unity VST3 AU AUv3 AAX VST LV2`, and there are
   no CLAP files in the tree [S5][S6]. Native CLAP was promised for JUCE 9 [S7]. It now depends on
   "AudioProcessor v2", which is "still some distance off" [S3]. For CLAP today, use **clap-juce-extensions**
   (MIT). It has supported JUCE 9 since 2026-07-21 (#178) [S8].
5. **Build-wise, the switch is low-risk for us.** CMake minimum 3.22 in both [S6]. C++17 minimum (20 with Windows
   MIDI Services), VS2019+, Win10 1607+ [S9]. The 9.0.0 breaking changes mostly touch Linux (EGL, libxi), SVG and
   `Drawable` (no longer a `Component`). On Windows, multi-touch is now off by default [S10]. ASIO headers are
   bundled in both lines from 8.0.11 on (see `asio-low-latency.md`).
6. **iPlug2 is a credible permissive alternative, but not a better fit.** zlib-like license, CLAP/VST3/AAX/standalone,
   ASIO through RtAudio [S11][S12]. On the other side: the only release tag is `v1.0.0-beta` (2024-11-10) [S12], the
   UI toolkit is smaller, and RtAudio bundles the **full** ASIO SDK sources [S12] (license of that copy: §4 Q4).
7. **DPF (ISC) and the CLAP-first route (clap-wrapper MIT, nih-plug ISC/Rust) do not fit.** DPF's Windows standalone
   is WASAPI/MME through RtAudio with no ASIO [S13]. clap-wrapper's standalone can add ASIO via RtAudio, but means
   writing our own UI and device layer [S14]. nih-plug is Rust, while our core is C++20 (decision in ADR-0001).
8. **pluginval** (GPL-3.0, last release v1.0.4, 2024-12-04) validates any VST3, whatever framework built it [S15].

## 2. Findings

### 2.1 JUCE 8 vs JUCE 9

| Aspect | JUCE 8 (8.0.15 / 8.0.16-security) | JUCE 9 (9.0.4) | Source |
|---|---|---|---|
| Release dates | 8.0.0 2024-06-12; 8.0.15 2026-07-21; 8.0.16-security 2026-10-13 | 9.0.0 2026-07-21; 9.0.4 2026-10-12 (monthly-ish point releases) | [S1] |
| Open-source licence | AGPLv3 / JUCE 8 EULA | AGPLv3 / JUCE 9 EULA (same wording otherwise) | [S2] |
| Pricing (commercial) | Starter free ≤ $20k, Indie ≤ $300k, Pro unlimited | "identical to JUCE 8". JUCE 9 EULA published 2026-06-17 | [S3][S4] |
| Maintenance | Security-only patches after 2026-07-21 (one so far). No published end-of-life policy | Active feature and bug-fix releases | [S1]; support policy *unverified* |
| CMake | `cmake_minimum_required(VERSION 3.22)` | 3.22. 9.0.0 "Improved the CMake build system for headless environments"; juceaide has no dependencies | [S6][S1][S3] |
| C++ / toolchain | C++17 | C++17 (C++20 with `JUCE_USE_WINDOWS_MIDI_SERVICES`), VS2019+, Win10 1607+, x64/Arm64/Arm64EC | [S9] |
| ASIO | Bundled headers from 8.0.11 | Bundled headers, `JUCE_ASIO` | [S16] |
| WASAPI | Shared / Low Latency (IAudioClient3) / Exclusive | Same code path | [S16] |
| VST3 | VST3 SDK (MIT) bundled | Same; 9.0.2 "Fixed some VST3 hosting issues" | [S2][S1] |
| CLAP | Not native; clap-juce-extensions | Not native in 9.0.4; clap-juce-extensions supports JUCE 9 (#178, plus a JUCE 9 editor-scale fix #183) | [S5][S8] |
| Graphics | 8.0.0: new **Direct2D renderer**, WebView UIs, animation module | + new SVG parser (lunasvg), variable fonts, faster software renderer, OpenGL perf (9.0.2) | [S1][S3] |
| HiDPI | Per-monitor scaling (*unverified* details) | 9.0.4 "Fixed Windows window placement when the display scale changes" | [S1] |
| Plugin API future | — | AudioProcessor v2 (parameters by value on the derived type, sample-accurate automation, CLAP features) arrives in 9.x with a compatibility layer | [S3][S7] |
| Breaking changes that hit us | — | Windows multi-touch off by default. `Drawable` no longer a `Component` (use `DrawableComponent`). `createFromSVG(XmlElement)` removed. 9.0.2: `ThreadPool::addJob` overloads changed | [S10] |

### 2.2 JUCE 9 EULA (applies only without the AGPL option)

| Clause | Content | Source |
|---|---|---|
| 1.15 | A new major version is not included in an existing licence | [S4] |
| 2.3 | Licensees must not make the Framework subject to a copyleft licence. This conflicts with AGPL use, so AGPL users simply don't take the EULA | [S4][S2] |
| 10.1–10.3 | Usage data collection under the privacy policy; publicity (opt-out); audit within 30 days | [S4] |
| Splash screen | Not mentioned in the JUCE 9 EULA | [S4] |

### 2.3 Alternatives

| Framework | License | Formats | Windows standalone + ASIO | UI | Maturity signals | Source |
|---|---|---|---|---|---|---|
| **JUCE 9** | AGPLv3 / commercial | VST3, AU, AUv3, AAX, LV2, Standalone (+CLAP via extension) | Yes: ASIO + 3 WASAPI modes, device selector UI built in | Full widget toolkit, Direct2D, WebView, SVG | Regular releases, 2026 | [S1][S5][S16] |
| **iPlug2** | zlib-like (WDL, NanoVG: zlib; RtAudio: MIT; Skia optional: BSD) | CLAP, VST2, VST3, AUv2, AUv3, AAX, WAM, standalone win32/macOS | Yes, via RtAudio (`kDeviceASIO`). RtAudio bundles `asio.cpp`, `asio.h`, `asiodrivers.*`, ... | IGraphics on NanoVG or Skia (GPU, HiDPI), or WebView/SwiftUI | 2.4k stars, active commits, only tag `v1.0.0-beta` (2024-11-10), root `CMakeLists.txt` present | [S11][S12] |
| **DPF** | ISC (format-specific extras in LICENSING.md) | LADSPA, DSSI, LV2, VST2, VST3, CLAP; JACK/standalone "to quickly test plugins" | Standalone uses RtAudio with WASAPI/MME. No ASIO in the bridge | DGL (OpenGL/Cairo) | Active, no releases | [S13] |
| **clap-wrapper** (CLAP-first) | MIT | CLAP → VST3, AUv2, AUv3, standalone, WCLAP | Standalone via RtAudio, ASIO only with `RTAUDIO_API_ASIO=ON` (ASIO SDK GPL3 or proprietary) | None: bring your own | v0.17.0 2026-10-20 | [S14] |
| **nih-plug** | ISC | CLAP, VST3 (Rust) | Standalone via its own backends (*unverified* ASIO) | egui/iced/VIZIA | Last push 2026-09-03, no releases | repo |
| **CLAP SDK** | MIT | — | — | — | Header-only ABI | repo |

## 3. Recommendations

| # | Recommendation | Verdict / parameters |
|---|---|---|
| R1 | **Keep JUCE, pin JUCE 9.0.4** (exact tag via FetchContent `GIT_TAG 9.0.4`, `GIT_SHALLOW ON`). Bump point releases in their own PRs, with goldens and pluginval | Licence: AGPL-3.0 option, compatible with our AGPL-3.0-or-later. No EULA obligations |
| R2 | ADR-0001 text: replace "JUCE 8" with "JUCE 9 (≥ 9.0.4)", and note "same dual licence as JUCE 8, verified 2026-10-25" | Orchestrator owns the ADR edit |
| R3 | Don't start on JUCE 8: it is security-only now, and a later 8→9 port would cost us a PR for no gain | — |
| R4 | CLAP is optional for v1. If wanted, add **clap-juce-extensions** pinned to a commit ≥ #178 (2026-07-21), and record it in `THIRD_PARTY.md` (MIT). Revisit when JUCE ships native CLAP with AudioProcessor v2 | MIT, compatible |
| R5 | Write parameter code so it can move to APv2: one parameter-definition table in `engine/`, mapped onto JUCE parameters in `app/`. This also keeps `dsp-core/` JUCE-free | — |
| R6 | UI: use JUCE's Direct2D renderer (added in 8.0.0; whether it is the Windows default is *unverified*) and vector drawing. Avoid `Drawable`-as-`Component` (removed in 9) | — |
| R7 | Keep iPlug2 as the documented fallback if the JUCE licence ever changes. No action now | zlib; would need an ASIO-licence review of RtAudio's bundled SDK copy |
| R8 | CI: build pluginval from its tag or download the v1.0.4 binary at CI time (GPL-3.0, tool only, not shipped) | Not distributed with our product |

## 4. Open questions (for the architect)

1. Ship CLAP in v1 (via clap-juce-extensions), or wait for native JUCE CLAP? Recommendation: v1 = Standalone + VST3 only.
2. How do we track JUCE 9 point releases: on every release, or only for fixes we need? A bump changes goldens only if
   graphics or DSP code changes. Audio processing is ours.
3. Do we enable `JUCE_USE_WINDOWS_MIDI_SERVICES` (needs C++20, which we already use)? MIDI is out of scope for v1
   (*unverified*: check against `docs/spec/`).
4. iPlug2 fallback only: which ASIO SDK version and license does RtAudio's bundled copy carry? *Unverified*. Not needed
   unless R7 triggers.
5. Guard list: this PR adds host and vendor names (Cockos, Avid, Bitwig). It does **not** add JUCE, iPlug2, DPF or CLAP,
   which `THIRD_PARTY.md`/CMake need. Is that acceptable?

## 5. Sources (all accessed 2026-10-25)

- [S1] JUCE releases (tags, dates, notes for 8.0.0, 8.0.15, 8.0.16-security, 9.0.0–9.0.4): https://github.com/juce-framework/JUCE/releases
- [S2] JUCE `LICENSE.md` at 8.0.15 and 9.0.4 (diffed): https://github.com/juce-framework/JUCE/blob/8.0.15/LICENSE.md ; https://github.com/juce-framework/JUCE/blob/9.0.4/LICENSE.md
- [S3] JUCE forum, "JUCE 9 is Available Now" (2026-07-21): https://forum.juce.com/t/juce-9-is-available-now/69175
- [S4] JUCE 9 End User Licence Agreement: https://juce.com/legal/juce-9-licence/
- [S5] JUCE 9.0.4 CMake API (`FORMATS`): https://github.com/juce-framework/JUCE/blob/9.0.4/docs/CMake%20API.md
- [S6] JUCE 9.0.4 and 8.0.15 trees / `CMakeLists.txt`: https://github.com/juce-framework/JUCE/tree/9.0.4
- [S7] JUCE Roadmap Update Q3 2025: https://juce.com/blog/juce-roadmap-update-q3-2025/
- [S8] clap-juce-extensions (MIT; commits #178 "JUCE 9 support", #183): https://github.com/free-audio/clap-juce-extensions ; https://github.com/free-audio/clap-juce-extensions/commits/main
- [S9] JUCE 9.0.4 `README.md` (minimum requirements): https://github.com/juce-framework/JUCE/blob/9.0.4/README.md
- [S10] JUCE `BREAKING_CHANGES.md` (9.0.0–9.0.3): https://github.com/juce-framework/JUCE/blob/9.0.4/BREAKING_CHANGES.md
- [S11] iPlug2 `LICENSE.txt` and `README.md`: https://github.com/iPlug2/iPlug2/blob/master/LICENSE.txt ; https://github.com/iPlug2/iPlug2
- [S12] iPlug2 tree (`Dependencies/IPlug/RTAudio/include/asio*`, `IPlug/APP/IPlugAPP_host.h`, `CMakeLists.txt`) and tag `v1.0.0-beta`: https://github.com/iPlug2/iPlug2/tree/master ; https://github.com/iPlug2/iPlug2/tags
- [S13] DPF `README.md` and `distrho/src/jackbridge/RtAudioBridge.hpp`: https://github.com/DISTRHO/DPF
- [S14] clap-wrapper `README.md`, `cmake/wrap_standalone.cmake`, release v0.17.0: https://github.com/free-audio/clap-wrapper
- [S15] pluginval (GPL-3.0, release v1.0.4): https://github.com/Tracktion/pluginval
- [S16] JUCE `juce_audio_devices` sources (ASIO flag, bundled headers, WASAPI modes); see `docs/research/asio-low-latency.md` [S4][S5]: https://github.com/juce-framework/JUCE/tree/master/modules/juce_audio_devices
- nih-plug: https://github.com/robbert-vdh/nih-plug ; CLAP: https://github.com/free-audio/clap
