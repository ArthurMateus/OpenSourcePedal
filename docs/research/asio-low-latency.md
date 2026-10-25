# ASIO SDK licensing, low-latency ASIO and WASAPI-exclusive practice

- Issue: #8. Author: acct3/researcher (Moth). Date: 2026-10-25.
- Scope: ASIO SDK license and how JUCE consumes it; real-time practice on Windows (small buffers, MMCSS, page
  faults, denormals, timers, power); WASAPI as the fallback; how to measure and report round-trip latency.
- Source tags such as [S1] refer to §5. All sources were accessed on 2026-10-25.

## 1. Summary (for the architect)

1. **ASIO SDK 2.3.4 (2025-10-15) is dual-licensed: proprietary Steinberg ASIO License *or* "GPL Version 3".**
   The text has no "or any later version" clause, so treat it as **GPL-3.0-only**. MSYS2 packages it with the same
   reading [S1][S2]. It is compatible with our AGPL-3.0-or-later through GPLv3 §13 / AGPLv3 §13 [S3]. It is
   redistributable under the GPLv3 option. The proprietary option forbids redistribution, but we don't use it.
2. **We do not need to fetch the ASIO SDK.** Since JUCE **8.0.11**, JUCE bundles the three headers it needs
   (`asio.h`, `asiosys.h`, `iasiodrv.h`) together with Steinberg's dual-license `LICENSE.txt`, in
   `modules/juce_audio_devices/native/asio/`. Setting `JUCE_ASIO=1` is enough [S4][S5]. This supersedes the
   "FetchContent the SDK" part of ADR-0001 (see §4, Q1).
3. If we ever need the full SDK (`JUCE_ASIO_USE_EXTERNAL_SDK=1`), pin
   `https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip` with
   SHA-256 `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca` (8 910 208 bytes, measured here) [S2].
4. **Trademark:** under the GPLv3 option, using the "ASIO" name or logo is optional. If we use them, the usage
   guidelines apply, and "ASIO" must not appear in our product name [S1]. Recommendation: no logo. Use the plain word
   "ASIO" in the device menu and manual, with a trademark attribution line.
5. **Default buffer: 64 samples @ 48 kHz (1.33 ms per callback). Offer 32, 128 and 256.** At 64 samples our
   CPU budget (25 % of a core) is about 0.33 ms per callback. The real round-trip time is larger than 2 × buffer:
   one published measurement is 232 samples (5.3 ms) at 44.1 kHz / 64 on a good interface [S14].
6. **WASAPI fallback order:** "Windows Audio (Low Latency Mode)" (shared, IAudioClient3), then
   "Windows Audio (Exclusive Mode)", then plain shared (10 ms engine period). JUCE implements all three with
   event-driven callbacks and MMCSS "Pro Audio" [S5][S6][S7].
7. **Real-time hygiene on Windows:** set FTZ/DAZ per callback (`juce::ScopedNoDenormals`). Pre-allocate *and
   pre-touch* every buffer in `prepareToPlay`. Opt the process out of EcoQoS. Do **not** call `timeBeginPeriod`
   [S8][S9][S10][S11].
8. **Latency reporting:** show three numbers: driver input, driver output (both from `ASIOGetLatencies`, which
   already include one buffer [S2]) and our processing latency (`setLatencySamples`). Ship a loopback
   **Measure round-trip** tool, because drivers, and universal wrappers especially, may report wrong numbers [S12][S13].
9. Universal ASIO wrappers (ASIO4ALL, FlexASIO) are only a fallback for users without a native driver. Our native
   WASAPI modes cover the same need with fewer moving parts [S12][S15][S16].

## 2. Findings

### 2.1 ASIO SDK license and distribution

| Item | Finding | Source |
|---|---|---|
| Current version | 2.3.4. `changes.txt`: "new DUAL licensing: Proprietary and GPVv3 [sic]; updated usage guidelines document" | [S2] |
| License text | "licensed under the terms of the Steinberg ASIO License, or alternatively under the terms of the General Public License (GPL) Version 3" | [S2] (`LICENSE.txt`) |
| GPL version | "Version 3", no "or later" clause. MSYS2 uses `spdx:GPL-3.0-only` | [S1][S2] |
| Proprietary option | "may not be distributed in parts or its entirety without prior written agreement". Also needs a signed agreement before publishing | [S2] |
| Headers redistributable? | Yes under GPLv3 (GPLv3 §4–6). JUCE already redistributes them in its AGPL/commercial repo | [S3][S4] |
| Official download | `https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip`. SHA-256 above, measured with `sha256sum` on 2026-10-25. It matches the MSYS2 PKGBUILD | [S1][S2] |
| Official Git repo | None found. The Steinberg developer pages rendered without SDK content for our fetcher | [S17] |
| Trademark | Optional under GPLv3. If used: unaltered logos, follow the *Usage Guidelines*, only in product contexts (splash, about, website, manual). Prohibited: "ASIO" in company or product names | [S1] (SDK `README.md`) |
| AGPL combination | GPLv3 §13 lets you combine with AGPLv3 works. Each part keeps its own license | [S3] |
| Consequence of GPL-3.0-only | Binaries with ASIO enabled cannot move to a future "AGPLv4" while they contain the SDK headers. Our own source stays "-or-later". *Unverified legal reading, not legal advice* | — |

### 2.2 How JUCE consumes ASIO (JUCE `master`, latest tag 9.0.4 of 2026-10-12)

| Item | Finding | Source |
|---|---|---|
| Flag | `JUCE_ASIO` (default 0) in `juce_audio_devices.h`. "your resulting program must adhere to the license terms of those files" | [S5] |
| Bundled headers | Commit "ASIO: Bundle required headers from SDK" (2025-10-21, `89fc608`). The first tag containing it is 8.0.11 (8.0.10 does not) | [S4] |
| External SDK | `JUCE_ASIO_USE_EXTERNAL_SDK=1` + include path to the SDK `common/` folder (must contain `iasiodrv.h`) | [S5] |
| Typical CMake | `target_compile_definitions(OpenSourcePedal PUBLIC JUCE_ASIO=1)` on the plugin target (shared code, so Standalone gets it). The VST3 never opens devices, so the flag is harmless there | [S5]; [S18] (forum, older JUCE) |
| Latencies | JUCE calls `getLatencies()` and exposes `getInputLatencyInSamples()` / `getOutputLatencyInSamples()` | [S5] (`juce_ASIO_windows.cpp`) |
| Guard impact | `tools/guard/check_repo.py` flags committed files named `asio.h`, `iasiodrv.h`, ... JUCE is fetched at configure time, so nothing changes | repo |

### 2.3 Real-time practice on Windows

| Topic | Finding | Source |
|---|---|---|
| Buffer math | Period = N / fs: 32 → 0.67 ms, 64 → 1.33 ms, 128 → 2.67 ms at 48 kHz. Round trip ≈ in + out driver latency (each includes ≥ 1 buffer) + converters + our processing | [S2] (`ASIOGetLatencies`: "usually the size of one block in sample frames, plus device specific latencies") |
| Measured example | 232 samples (≈ 5.3 ms) round trip at 44.1 kHz / 64-sample ASIO buffer on one high-end interface | [S14] |
| MMCSS | Task "Pro Audio" uses scheduling category High, priority 23–26, "designed for Pro Audio tasks". Join with `AvSetMmThreadCharacteristics` + `AvSetMmThreadPriority` | [S7] |
| MMCSS in JUCE | The WASAPI thread calls `AvSetMmThreadCharacteristicsW(L"Pro Audio")` + `AVRT_PRIORITY_NORMAL`. With ASIO, the callback thread belongs to the **driver**. The SDK 2.3 PDF documents MMCSS on p. 46 (we could not extract the text) | [S5][S2] |
| SystemResponsiveness | Registry value. 100 disables MMCSS, < 10 clamps to 20. It is user or admin territory, not ours | [S7] |
| Page faults | `VirtualLock` guarantees no page fault, but the per-process limit is "intentionally small". Raise it with `SetProcessWorkingSetSize` first | [S9] |
| Denormals | `juce::ScopedNoDenormals` / `FloatVectorOperations::disableDenormalisedNumberSupport` "sets the DAZ and FZ bits of the MXCSR register". On ARM it sets the equivalent FPCR bit | [S8] |
| Timer resolution | Since Win10 2004, `timeBeginPeriod` is per-process. On Win11 it is ignored when the window is occluded or minimized *and inaudible*. The audio callback is driven by driver events, so it doesn't need timers | [S10] |
| Power / QoS | `SetProcessInformation(ProcessPowerThrottling)` with `ControlMask=EXECUTION_SPEED, StateMask=0` = HighQoS (no EcoQoS). Otherwise Windows infers the QoS heuristically | [S11] |
| Power plan, USB selective suspend, C-states | Common forum advice for DAW users. *Unverified*: we found no primary source | — |
| Windows low-latency mode | With small buffers, Windows "enters a special mode" to isolate audio resources (WASAPI/AudioGraph path) | [S6] |

### 2.4 WASAPI as fallback

| Mode (JUCE device type) | Latency | Notes | Source |
|---|---|---|---|
| Shared, "Windows Audio" | Engine period 10 ms by default (+ engine ≈ 1.3 ms render on Win10) | Other apps keep sound. Resampling possible | [S6] |
| Shared low latency, "Windows Audio (Low Latency Mode)" | Any multiple of the fundamental period in [min, max] from `GetSharedModeEnginePeriod`. The inbox HDAudio driver goes down to 128 samples (2.66 ms @ 48 kHz) | Needs Win10 + a driver that declares small packets. `AUDCLNT_E_ENGINE_PERIODICITY_LOCKED` if another app holds a different period | [S6][S5] |
| Exclusive, "Windows Audio (Exclusive Mode)" | Down to the device minimum period (`GetDevicePeriod`). JUCE uses max(minPeriod, requested) and handles the buffer-alignment retry | Silences all other apps on that endpoint. Bit-exact and custom rates | [S5][S19] |
| All JUCE modes | Event-driven (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, `0x40000`) | — | [S5] |

### 2.5 Universal ASIO wrappers

| Driver | Facts | Source |
|---|---|---|
| ASIO4ALL 2.22 (2026-07-23) | Universal ASIO driver over WDM. Its own docs say it "can only guess" the real latencies, so users should calibrate with a loopback measurement | [S12][S15] |
| ASIO4ALL single-client | Not multi-client, so other apps go silent. *Forum claim* | [S20] |
| FlexASIO 1.10b (2024-06-02) | Universal ASIO driver on PortAudio. Backends: DirectSound (default), MME, WASAPI shared/exclusive, KS. Default buffer 20 ms. Low-latency WASAPI exclusive configs are "experimental". The license is not identified by the GitHub API (NOASSERTION) | [S16][S21] |

### 2.6 Measuring round-trip latency

| Method | Finding | Source |
|---|---|---|
| Loopback | Patch a cable from output to input, turn off direct monitoring, play a known signal and find it in the capture | [S6][S13] |
| JUCE `AudioLatencyDemo` | Test signal of **randomly spaced ±0.99 spike pairs**. It detects spikes above 5× the running average, matches the pattern, then reports measured − (device in + device out) as "corrected latency" | [S22] |
| Commercial-free tool | RTL Utility measures interface + driver round trip. Its v1.0 detection works on noisy loopbacks | [S13] |
| Lower bound | Measured RTL is a lower bound. Processing only adds delay | [S23] |
| MLS / log-sweep cross-correlation | More noise-robust than spikes and gives sub-sample precision through peak interpolation. *Unverified here*: no primary source fetched | — |

## 3. Recommendations

| # | Recommendation | License verdict / parameters |
|---|---|---|
| R1 | Require **JUCE ≥ 8.0.11** and set `JUCE_ASIO=1`, `JUCE_ASIO_USE_EXTERNAL_SDK=0`, `JUCE_WASAPI=1`. No ASIO FetchContent | Headers: GPL-3.0-only (ASIO option) inside JUCE. OK with AGPL-3.0-or-later. Add a row to `THIRD_PARTY.md`: "ASIO SDK 2.3.4 headers (via JUCE), Steinberg, GPL-3.0-only" |
| R2 | Keep the pinned URL + SHA-256 from §1.3 in `cmake/` as a commented fallback only | — |
| R3 | No ASIO logo. Device menu text "ASIO". About box / manual line: "ASIO is a trademark and software of Steinberg Media Technologies GmbH" (common wording; *unverified* against the Usage Guidelines PDF, so check it before release) | Trademark rules [S1] |
| R4 | Buffer sizes offered: 32, 64 (default), 96, 128, 192, 256, 512. Sample rates 44.1/48/88.2/96 kHz. Default 48 kHz / 64 | Budget: chain < 0.33 ms per 64-sample callback |
| R5 | Device-type order in the UI: ASIO (if present) → WASAPI Low Latency → WASAPI Exclusive → WASAPI shared. Warn that exclusive mode silences other apps | — |
| R6 | First lines of `processBlock`: `juce::ScopedNoDenormals`. `dsp-core` gets its own `ScopedFlushDenormals` (MXCSR bits 6 + 15 on x86, FPCR.FZ on ARM) so it stays JUCE-free | — |
| R7 | `prepareToPlay`: allocate everything, then **write zeros through every buffer** (pre-touch), including IR/NAM weights and delay lines. Optional: `SetProcessWorkingSetSize` + `VirtualLock` on the hot arena (< ~16 MB), failure tolerated | Limit is small [S9] |
| R8 | Standalone start-up: opt out of EcoQoS (HighQoS). Don't touch `timeBeginPeriod` or the power plan; document plan/USB tips as user advice (*unverified*) | [S11] |
| R9 | Worker threads we create (e.g. parallel cab/NAM) join MMCSS "Pro Audio" like JUCE's WASAPI thread. Never call MMCSS APIs from inside the audio callback | [S7] |
| R10 | Latency panel: input / output (driver-reported, samples + ms), processing (`setLatencySamples`), predicted total, and **measured** total from a loopback test using the JUCE spike-pattern method first (MLS later). Store the measured offset per device + buffer size | [S2][S22] |
| R11 | CI can't test ASIO hardware. Unit-test the latency detector offline: synthesise a capture = delayed + noisy test signal and assert ±1 sample at SNR ≥ 20 dB | — |

## 4. Open questions (for the architect)

1. **ADR-0001 update:** replace "fetch the SDK at configure time" with "use JUCE's bundled ASIO headers (JUCE ≥ 8.0.11)"?
   ADR-0001 also names a note `docs/research/asio-sdk-licensing.md`. This file (`asio-low-latency.md`, per issue #8)
   takes that role.
2. **JUCE major version:** ADR-0001 says JUCE 8, but JUCE 9.0.4 (2026-10-12) is current. Re-check the JUCE 9
   license and pick the pin in #9 (framework comparison).
3. Guard word list: this PR adds measurement-tool and vendor names that have no reason to appear in the product
   (RTL Utility, Oblique Audio, Native Instruments, RME). It does **not** add Steinberg, ASIO, ASIO4ALL or FlexASIO,
   because `THIRD_PARTY.md`, CMake and the user manual need them for interoperability. Do you agree?
4. Does the `tools/guard` ASIO-file check need an allow-list entry, in case we ever vendor JUCE?
   Today nothing is vendored, so the answer is no.
5. Should the latency measurement live in `app/` only, or as a reusable detector in `engine/` + a CLI in `tools/`?
6. Should `VirtualLock` (R7) be on by default or behind an "advanced" toggle?

## 5. Sources (all accessed 2026-10-25)

- [S1] MSYS2 PKGBUILD `mingw-w64-asiosdk`: https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-asiosdk ; https://packages.msys2.org/base/mingw-w64-asiosdk. SDK `README.md` read from [S2]
- [S2] Steinberg ASIO SDK 2.3.4 archive (`LICENSE.txt`, `README.md`, `changes.txt`, `common/asio.h`): https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip. Downloaded to a scratch directory and inspected, not committed
- [S3] GNU GPLv3 (§13 "Use with the GNU Affero General Public License"): https://www.gnu.org/licenses/gpl-3.0.html
- [S4] JUCE commit "ASIO: Bundle required headers from SDK": https://github.com/juce-framework/JUCE/commit/89fc608a347063a477c1d23e32a533fb5a6fea7c ; bundled license: https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/asio/LICENSE.txt
- [S5] JUCE sources: https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/juce_audio_devices.h ; .../native/juce_ASIO_windows.cpp ; .../native/juce_WASAPI_windows.cpp ; releases: https://github.com/juce-framework/JUCE/releases
- [S6] Microsoft, Low Latency Audio (updated 2025-03-26): https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio
- [S7] Microsoft, Multimedia Class Scheduler Service: https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service
- [S8] JUCE `juce_FloatVectorOperations.h`: https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_basics/buffers/juce_FloatVectorOperations.h
- [S9] Microsoft, VirtualLock: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock
- [S10] Microsoft, timeBeginPeriod: https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
- [S11] Microsoft, SetProcessInformation: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation
- [S12] ASIO4ALL, Latency Compensation: https://asio4all.org/latency-compensation/
- [S13] Gearnews, RTL Utility official release: https://www.gearnews.com/rtl-utility-for-measuring-audio-latency-gets-an-official-release/ ; Sound On Sound RTL technique: https://www.soundonsound.com/techniques/round-trip-latency-rtl-j-scope-oscilloscope
- [S14] Archimago, RTL Utility measurements (2021-11-06): https://archimago.blogspot.com/2021/11/rtl-utility-look-at-audio-interface.html
- [S15] ASIO4ALL home: https://www.asio4all.org/
- [S16] FlexASIO repo and configuration: https://github.com/dechamps/FlexASIO ; https://github.com/dechamps/FlexASIO/blob/flexasio-1.0/CONFIGURATION.md
- [S17] Steinberg developer pages: https://www.steinberg.net/developers/ ; https://www.steinberg.net/developers/asiosdk-open/ ; press coverage: https://heise.de/-10963451 ; https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/
- [S18] JUCE forum, enabling JUCE_ASIO in CMake: https://forum.juce.com/t/how-to-enable-juce-asio-in-cmake/48407
- [S19] Microsoft, Exclusive-Mode Streams: https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams
- [S20] NI community thread on ASIO4ALL: https://community.native-instruments.com/discussion/comment/43969/
- [S21] FlexASIO latest release (GitHub API): https://github.com/dechamps/FlexASIO/releases
- [S22] JUCE AudioLatencyDemo: https://github.com/juce-framework/JUCE/blob/master/examples/Audio/AudioLatencyDemo.h
- [S23] MathWorks, Measure Audio Latency: https://www.mathworks.com/help/audio/ug/measure-audio-latency.html
