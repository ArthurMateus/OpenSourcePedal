# Research: Measurement and Test Tooling for Audio DSP

- **Issue:** #13
- **Author:** `gemini/researcher` (Corvid)
- **Date:** 2026-10-25

---

## 1. Summary

1. **LUFS (ITU-R BS.1770-4 / EBU R128):** Use **`jiixyj/libebur128`** (C, **MIT**, AGPLv3-compatible) inside C++ tools (`osp-render` / Catch2 tests) and **`csteinmetz1/pyloudnorm`** (Python, **MIT**) in offline Python analysis scripts. Both implement ITU-R BS.1770-4 K-weighting and gated integration; self-implementing is unnecessary except as a 2-biquad verification reference.
2. **Golden-render regression:** Use a **two-tier gate** in `ctest`:
   - **Tier 1 (same-platform deterministic CI):** Sample-accurate **null test** after `getLatencySamples()` alignment; require peak/RMS residual $\le -120\text{ dBFS}$ for linear/classic-DSP blocks and bypass paths.
   - **Tier 2 (cross-compiler / SIMD / fast-math / neural blocks):** **Log-Spectral Distance (LSD)** ($\le 0.5\text{ dB}$ mean, $\le 1.5\text{ dB}$ max across 1/3-octave bands) plus **Multi-Resolution STFT (MR-STFT)** convergence ($\le 10^{-3}$, reference implementation: `csteinmetz1/auraloss`, **Apache-2.0**).
3. **THD, THD+N, and aliasing:** Combine **stepped sine + 4-term Blackman-Harris FFT** (coherent bin frequencies at $997\text{ Hz}$ for THD/THD+N and $5\text{ kHz}$–$15\text{ kHz}$ for folded aliasing below fundamental) with **Farina Exponential Sine Sweep (ESS)** deconvolution (AES 2000) to capture continuous frequency response and harmonic impulse orders $H_2 \dots H_5$ in one pass.
4. **Real-time safety on Windows/MSVC:** Clang 20's **RealtimeSanitizer (`-fsanitize=realtime` / RTSan)** supports Linux, macOS, and iOS only—**Windows is unsupported** as of LLVM 20. On Windows/MSVC, enforce RT safety via a **global `operator new` / `operator delete` override** paired with a `thread_local` audio-thread guard, supplemented in Debug builds by MSVC **`_CrtSetAllocHook`** (catches C `malloc`/`calloc`/`realloc` inside third-party libs), custom `AudioSpinLock` / mutex wrapper assertions, and denormal checks (`_MM_GET_FLUSH_ZERO_MODE`, `_MM_GET_DAZ_MODE`, plus `std::fpclassify` output buffer assertions).
5. **Plugin validation (`pluginval`):** Fetch the prebuilt Windows binary (`pluginval_Windows.zip`, **GPLv3**) from `Tracktion/pluginval` GitHub Releases in CI and run `pluginval.exe --strictness-level 8 --validate-in-process --timeout-ms 60000 --validate <path-to-.vst3>`. Level 8+ enforces binary state round-tripping (`getStateInformation`/`setStateInformation`) and discrete parameter restoration.
6. **Benchmarking & CI noise normalization:** Use **`martinus/nanobench`** (single-header C++, **MIT**) for `osp-bench`. GitHub Actions `windows-latest` shared VMs exhibit $15\%$–$30\%$ wall-clock jitter (*unverified* exact runner variance; mitigated by normalization): normalize every block/preset benchmark against a pinned **reference DSP kernel** (fixed 64-biquad cascade run in the same process) in addition to reporting real-time CPU budget % ($\text{wall time} / (N / f_s) < 25\%$ at $48\text{ kHz}$ / 64 samples).

---

## 2. Findings

### 2.1 LUFS Loudness Measurement (ITU-R BS.1770-4 / EBU R128)

ITU-R BS.1770-4 defines integrated loudness (LKFS/LUFS) via a two-stage **K-weighting filter** (Stage 1: high-shelf head-acoustics filter $+4\text{ dB}$ above $\sim 1.5\text{ kHz}$; Stage 2: RLB 2nd-order high-pass at $\sim 38\text{ Hz}$), $400\text{ ms}$ rectangular gating blocks ($75\%$ overlap, $100\text{ ms}$ step), an absolute gate at $-70\text{ LUFS}$, and a relative gate at $-10\text{ LU}$ below the ungated mean ([ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en), accessed 2026-10-25; [EBU Tech 3341](https://tech.ebu.ch/publications/tech3341), accessed 2026-10-25).

| Option | Language | License | AGPLv3-Compatible? | Redistributable? | Pros / Cons | Source |
|---|---|---|---|---|---|---|
| **`jiixyj/libebur128`** | C (C99) | **MIT** | Yes | Yes | Single small C library (~2 files); supports any sample rate (interpolated filter coefficients + true-peak oversampling); zero external dependencies; battle-tested in FFmpeg/Ardour. | [github.com/jiixyj/libebur128](https://github.com/jiixyj/libebur128) (2026-10-25) |
| **`csteinmetz1/pyloudnorm`** | Python | **MIT** | Yes | Yes | Clean NumPy/SciPy implementation of ITU-R BS.1770-4; ideal for `tools/` preset loudness linters. | [github.com/csteinmetz1/pyloudnorm](https://github.com/csteinmetz1/pyloudnorm) (2026-10-25) |
| **Custom C++20 implementation** | C++20 | AGPL-3.0-or-later | Yes | Yes | ~120 LOC (2 biquads + ring buffer for $400\text{ ms}$ blocks + two-pass histogram gating). ITU paper gives exact $48\text{ kHz}$ coefficients; other rates require bilinear/Analytical design (e.g. Mansbridge et al. 2012). | [ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en) (2026-10-25) |

---

### 2.2 Golden-Render Regression: Null Tests & Spectral Distance Metrics

Audio DSP golden-render regression must distinguish unintended bugs from benign floating-point reassociation across compiler versions or SIMD widths:

| Metric | Mathematical Definition | Target Tolerance | Best Use Case | Source |
|---|---|---|---|---|
| **Sample-aligned Null Test** | $E_{\text{null}} = 10\log_{10}\!\left(\frac{1}{N}\sum_{n} (y[n] - y_{\text{ref}}[n])^2\right)$ and peak $\max_n |y[n]-y_{\text{ref}}[n]|$ | RMS $\le -120\text{ dBFS}$, Peak $\le -110\text{ dBFS}$ (same OS/compiler); $\le -90\text{ dBFS}$ (cross-SIMD) | Bypass verification (`AGENTS.md` §4), deterministic regression on `windows-latest` MSVC. | Industry practice; *unverified* universal standard threshold (`AGENTS.md` specifies $\le -120\text{ dBFS}$ for bypass) |
| **Log-Spectral Distance (LSD)** | $\text{LSD} = \frac{1}{T}\sum_{t=1}^{T}\sqrt{\frac{1}{K}\sum_{k=1}^{K}\left[10\log_{10}\frac{|Y(t,k)|^2 + \epsilon}{|Y_{\text{ref}}(t,k)|^2 + \epsilon}\right]^2}$ ($\epsilon = 10^{-10}$, $-100\text{ dBFS}$ floor) | Mean $\le 0.2\text{ dB}$ (golden match), $\le 1.5\text{ dB}$ per octave band (tone target matching) | Insensitive to tiny phase/rounding shifts; catches filter cutoff, EQ, or gain regressions. | [Gray & Markel 1976 / Wikipedia LSD](https://en.wikipedia.org/wiki/Log-spectral_distance) (2026-10-25) |
| **Multi-Resolution STFT (MR-STFT)** | Sum of spectral convergence $\frac{\|\,|Y|-|Y_{\text{ref}}|\,\|_F}{\|\,|Y_{\text{ref}}|\,\|_F}$ + log-magnitude $L_1$ across FFT sizes $\{512, 1024, 2048\}$ | Spectral convergence $\le 10^{-3}$ for golden regression | Captures both transient timing (short FFT) and steady-state harmonic accuracy (long FFT). Reference: `csteinmetz1/auraloss` (**Apache-2.0**). | [github.com/csteinmetz1/auraloss](https://github.com/csteinmetz1/auraloss) (2026-10-25) |

**Tolerance-setting practice:** Always silence-floor clamp magnitudes at $\epsilon = 10^{-10}$ ($-100\text{ dBFS}$) before taking logarithms so quiet noise-gate tails do not cause division-by-zero spikes in LSD. Align latency via `getLatencySamples()` before computing time-domain residuals.

---

### 2.3 THD, THD+N, and Aliasing Measurement Methods

1. **Coherent Stepped/Single-Tone FFT (with 4-term Blackman-Harris window):**
   - Choose test frequency $f_0 = \frac{m}{N_{\text{FFT}}} f_s$ (with odd integer $m$, e.g. near $997\text{ Hz}$, $N_{\text{FFT}} = 65536$) or apply a **4-term Blackman-Harris window** (sidelobe floor $-92\text{ dB}$) to suppress spectral leakage ([Audio Precision THD+N note](https://www.ap.com/technical-library/more-about-thdn/), accessed 2026-10-25; [Harris 1978 window paper](https://ieeexplore.ieee.org/document/1455106), accessed 2026-10-25).
   - **THD:** $\sqrt{\sum_{k=2}^{H} A_k^2} / A_1$ ($H=10$, or harmonics up to $f_s/2$).
   - **THD+N:** Total RMS energy from $20\text{ Hz}$ to $20\text{ kHz}$ with the fundamental bin ($f_0 \pm 3$ bins) notched out, divided by total signal RMS.
   - **Aliasing (Signal-to-Aliasing Ratio / SAR):** Drive nonlinear block (overdrive, amp) with a high-frequency tone ($f_0 = 5432\text{ Hz}$ or stepped $2\text{ kHz}$–$15\text{ kHz}$ at $-6\text{ dBFS}$). Any non-harmonic bin $f \neq k f_0$ inside $20\text{ Hz}$–$20\text{ kHz}$ (especiallybins *below* $f_0$, which can only be folded aliases $|m f_s - k f_0|$) is summed as aliasing power $P_{\text{alias}}$. Oversampling verification asserts $\text{SAR} = 10\log_{10}(P_1 / P_{\text{alias}}) \ge 70\text{ dB}$ (*unverified* project threshold; to be fixed in `docs/spec/`).
2. **Farina Exponential Sine Sweep (ESS):**
   - Stimulus: $x(t) = \sin\!\left[\frac{2\pi f_1 T}{\ln(f_2/f_1)}\left(e^{\frac{t}{T}\ln(f_2/f_1)} - 1\right)\right]$ from $f_1 = 20\text{ Hz}$ to $f_2 = 20\text{ kHz}$ over $T = 2\text{ s}$ ([Farina, AES 108th Convention Paper 5093, 2000](https://www.aes.org/e-lib/browse.cfm?elib=10211), accessed 2026-10-25).
   - Convolving the output $y(t)$ with the time-reversed, $-6\text{ dB/oct}$ amplitude-weighted inverse filter separates the linear impulse response at $t = T$ from the $k$-th harmonic distortion impulse responses at negative time offsets $\Delta t_k = -T \frac{\ln k}{\ln(f_2/f_1)}$. Windowing each $\Delta t_k$ yields continuous frequency-dependent $H_2(f), H_3(f), \dots$ curves in one render.

---

### 2.4 Real-Time Safety Checkers on Windows / MSVC

| Mechanism | Windows / MSVC Support? | What It Catches | Details & Source |
|---|---|---|---|
| **Clang `RealtimeSanitizer` (`-fsanitize=realtime`, RTSan)** | **No** (Linux, macOS, iOS only as of Clang 20) | `malloc`/`free`, `pthread_mutex_lock`, syscalls, file I/O in `[[clang::nonblocking]]` functions | Upstreamed in LLVM/Clang 20, but `compiler-rt` interceptors do not support Windows/MSVC ([Clang 20 RealtimeSanitizer Docs](https://clang.llvm.org/docs/RealtimeSanitizer.html), accessed 2026-10-25; [realtime-sanitizer/rtsan](https://github.com/realtime-sanitizer/rtsan), accessed 2026-10-25). Optional secondary Linux/macOS CI job only. |
| **Global `operator new` / `delete` override + `thread_local` guard** | **Yes** (MSVC, Clang, GCC; Debug & Release-with-tests) | All C++ heap allocations (`new`, `std::vector`, `std::function` heap fallback) on the audio thread | Define `thread_local bool g_inAudioThread = false;` via RAII `ScopedNoAlloc` in `process()` / `processBlock()`. Overridden `::operator new(size_t)` increments an atomic violation counter or calls ` __debugbreak()` / fails the Catch2 test if `g_inAudioThread` is true. Standard C++20 ([cppreference `operator new`](https://en.cppreference.com/w/cpp/memory/new/operator_new), accessed 2026-10-25). |
| **MSVC `_CrtSetAllocHook`** | **Yes** (MSVC Debug builds `_DEBUG`) | All CRT heap calls (`malloc`, `calloc`, `realloc`, `free`, `new`, `delete`), including inside C libraries | `<crtdbg.h>` hook `int __cdecl AllocHook(int allocType, void*, size_t, int blockType, long, const unsigned char*, int)`. Ignore `_CRT_BLOCK`, flag `_HOOK_ALLOC`/`_HOOK_REALLOC`/`_HOOK_FREE` when `g_inAudioThread` is true ([Microsoft Learn `_CrtSetAllocHook`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/crtsetallochook), accessed 2026-10-25). |
| **Lock Detection Wrapper** | **Yes** (portable C++20) | Blocking mutex acquisitions on audio thread | Wrap non-RT mutexes or route audio-thread sync exclusively through `try_lock()` / lock-free SPSC queues; assert `!g_inAudioThread` in blocking lock wrappers. |
| **Denormal Detection & FTZ/DAZ Verification** | **Yes** (`<immintrin.h>` + `<cmath>`) | Subnormal float CPU spikes ($\sim 10\times$–$100\times$ slowdown on x86) and missing FTZ/DAZ guards | 1) Assert `_MM_GET_FLUSH_ZERO_MODE() == _MM_FLUSH_ZERO_ON` and `_MM_GET_DAZ_MODE() == _MM_DENORMALS_ZERO_ON` inside `process()`. 2) Feed $0.5\text{ s}$ impulse followed by $10\text{ s}$ of silence (or $10^{-38}$ noise) and scan every output sample with `std::fpclassify(x) != FP_SUBNORMAL && std::isfinite(x)` ([cppreference `fpclassify`](https://en.cppreference.com/w/cpp/numeric/math/fpclassify), accessed 2026-10-25). |

---

### 2.5 `pluginval` in Windows CI & Known JUCE Pitfalls

- **Repository & License:** [`Tracktion/pluginval`](https://github.com/Tracktion/pluginval) (accessed 2026-10-25), licensed under **GPLv3** (standalone validation CLI tool; invoked as an external process in CI, so no licensing impact on `dsp-core` or binary distribution).
- **Windows CI Download & Invocation:**
  ```powershell
  Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Windows.zip" -OutFile pluginval.zip
  Expand-Archive -Path pluginval.zip -DestinationPath tools/pluginval
  ./tools/pluginval/pluginval.exe --strictness-level 8 --validate-in-process --timeout-ms 60000 --output-dir build/pluginval-logs --validate "build/release/app/OpenSourcePedal_artefacts/Release/VST3/OpenSourcePedal.vst3"
  ```
- **What Strictness Levels 8–10 Enforce ([pluginval tests](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing.md), accessed 2026-10-25):**
  - Levels 1–5: Plugin scan, instantiate, sample rates ($44.1, 48, 96\text{ kHz}$), block sizes ($64 \dots 2048$), editor open/close, basic parameter fuzzing, automation.
  - **Level 8 (`AGENTS.md` gate):** **Parameter state restoration** (including discrete/bool parameters) and **binary state equivalence** (`getStateInformation` $\rightarrow$ `setStateInformation` $\rightarrow$ `getStateInformation` must match).
  - Levels 9–10: Multi-threaded concurrent background parameter changes during `processBlock()`, extreme block sizes, allocator interceptor checks (`--rtcheck enabled`).
- **Known JUCE `AudioProcessor` Pitfalls at Level $\ge 8$:**
  1. **Non-idempotent `getStateInformation` / `setStateInformation`:** Serializing uninitialized float fields, timestamps, or UI window sizes that mutate between calls breaks Level 8 binary comparison.
  2. **Discrete parameter drift:** `AudioParameterChoice` / `AudioParameterBool` values must snap identically when restored from XML/JSON state vs host normalized float automation.
  3. **Dangling `AudioProcessorValueTreeState` (APVTS) listeners:** Opening and rapidly closing the `AudioProcessorEditor` while parameters automate crashes if `removeParameterListener` is missed in component destructors.
  4. **Unprepared `processBlock` calls or odd block sizes:** Hosts (and `pluginval`) may pass `numSamples == 0`, `numSamples == 1`, or odd sizes up to `maxBlockSize`, or call `processBlock` with extra/disabled bus channels.

---

### 2.6 Benchmark Harnesses & CI Runner Noise Normalization

| Library | License | AGPLv3-Compatible? | Overhead / Features | Verdict | Source |
|---|---|---|---|---|---|
| **`martinus/nanobench`** | **MIT** | Yes | Single header (`nanobench.h`); fast auto-tuning; median & median-absolute-deviation (MAD) outlier detection; JSON/CSV/Markdown output. | **Recommended** for `osp-bench` | [github.com/martinus/nanobench](https://github.com/martinus/nanobench) (2026-10-25) |
| **`CatchOrg/Catch2` (`BENCHMARK`)** | **BSL-1.0** | Yes | Built into Catch2 (already planned for unit tests); convenient for quick inline checks, but slower calibration and no built-in stability warnings. | Secondary (inline sanity checks only) | [github.com/CatchOrg/Catch2](https://github.com/CatchOrg/Catch2) (2026-10-25) |
| **`google/benchmark`** | **Apache-2.0** | Yes | Industry standard, rich counters, Python `compare.py`, but heavier CMake build/link dependency than `nanobench`. | Viable alternative | [github.com/google/benchmark](https://github.com/google/benchmark) (2026-10-25) |

**Normalizing CI Runner Noise (`windows-latest`):**
Shared cloud VMs suffer from hypervisor scheduling jitter and turbo-boost frequency scaling. To prevent flaky $>10\%$ regression failures (`AGENTS.md` §4):
1. **Reference DSP Baseline Normalization:** At the start of `osp-bench`, run a pinned, deterministic reference workload $T_{\text{ref}}$: 64 cascaded RBJ biquad filters processing $48\,000$ samples ($1\text{ s}$ at $48\text{ kHz}$ in 64-sample blocks). Store each block's budget in `tests/bench/budget.json` both as **real-time CPU %** ($\text{CPU\%} = 100 \times T_{\text{block}} / T_{\text{audio\_duration}} < 25\%$) and as a **normalized ratio** $R = T_{\text{block}} / T_{\text{ref}}$. Gate CI regressions ($>10\%$) on the median of $R$ across $\ge 21$ epochs.
2. **Process Pinning on Windows:** Call `SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)` and `SetThreadAffinityMask(GetCurrentThread(), 1ULL)` inside `osp-bench` startup to minimize core-migration jitter ([Microsoft Learn `SetThreadAffinityMask`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask), accessed 2026-10-25).

---

## 3. Recommendations for OpenSourcePedal

| Category | Recommended Tool / Library | Version / Pin | License | Where Used | Concrete Parameters / Thresholds |
|---|---|---|---|---|---|
| **C++ Unit & RT Tests** | `CatchOrg/Catch2` | `v3.7.1` (or latest `v3.x`) | BSL-1.0 | `tests/` | Sample rates $\{44.1, 48, 88.2, 96\}\text{ kHz}$ $\times$ block sizes $\{0, 1, 32, 64, 127, 512, 2048\}$. |
| **LUFS (C++ & CLI)** | `jiixyj/libebur128` | `v1.2.6` | MIT | `tools/osp-render`, `tests/` | Integrated loudness target $-18.0\text{ LUFS-I} \pm 0.5\text{ LU}$; True Peak $\le -1.0\text{ dBTP}$. |
| **LUFS & Spectrum (Python)** | `csteinmetz1/pyloudnorm` + `scipy.signal` | `0.1.1` | MIT / BSD-3 | `tools/analyze.py` | 1/3-octave & full-octave RMS error ($\text{ dB}$), LSD ($\epsilon = 10^{-10}$), MR-STFT (` {512, 1024, 2048}`). |
| **Golden Regression** | Built-in `osp-render` + Catch2 golden runner | In-tree | AGPL-3.0-or-later | `tests/golden/` | Null test residual $\le -120\text{ dBFS}$ (bypass & deterministic blocks); LSD mean $\le 0.2\text{ dB}$ (cross-platform/neural). |
| **THD / Aliasing** | In-tree C++ FFT (or KissFFT / PFFFT) + Farina ESS | In-tree | BSD-3 / AGPLv3 | `tests/dsp/` | $997\text{ Hz}$ stepped tone (THD), $5432\text{ Hz}$ tone (aliasing SAR), $2\text{ s}$ $20\text{ Hz}$–$20\text{ kHz}$ ESS sweep. |
| **RT Safety Checker** | Global `operator new` override + `_CrtSetAllocHook` + FTZ/DAZ & `std::fpclassify` | In-tree (`tests/rt_checker.hpp`) | AGPL-3.0-or-later | `tests/` (Debug & Release) | 0 allocations, 0 frees, 0 blocking locks, 0 `FP_SUBNORMAL` / `FP_NAN` / `FP_INFINITE` samples in `process()`. |
| **Benchmarks** | `martinus/nanobench` | `v4.3.11` | MIT | `tests/osp-bench` | $\ge 21$ epochs, median normalized to 64-biquad reference kernel $T_{\text{ref}}$; full chain $< 25\%$ core at $48\text{ kHz}$ / 64 samples; fail CI on $>10\%$ normalized regression. |
| **Plugin Validation** | `Tracktion/pluginval` | `v1.0.3` (GitHub Release zip) | GPLv3 (external CLI) | `.github/workflows/ci.yml` | `pluginval.exe --strictness-level 8 --validate-in-process --timeout-ms 60000 --validate <vst3>`. |

---

## 4. Open Questions for the Architect

1. **In-tree FFT for C++ unit tests (`tests/dsp/`):** Should `tests/` use a tiny permissive single-file FFT (e.g. `pffft` BSD-like or `kissfft` BSD-3-Clause) shared with `dsp-core` (for partitioned IR convolution in Phase 4/5), or keep frequency-response/THD FFT evaluation in Python (`scipy`) vs C++ Catch2? *(Recommendation: share the same BSD/MIT C++ FFT chosen for the cab IR loader so `ctest` is 100% self-contained in C++).*
2. **Golden render storage format:** Store golden reference renders in `tests/golden/` (via Git LFS) as 32-bit float `.wav` (exact bit-level null testing) or 24-bit `.flac` (smaller LFS footprint, $-144\text{ dBFS}$ quantization floor)? *(Recommendation: 32-bit float `.wav` for short 2–3 s test clips in Git LFS to avoid quantization floor masking $-120\text{ dBFS}$ null tests).*
3. **Aliasing (SAR) pass/fail threshold per nonlinear block:** Should `docs/spec/requirements.md` mandate a uniform Signal-to-Aliasing Ratio (e.g. $\ge 60\text{ dB}$ at $-6\text{ dBFS}$, $5.4\text{ kHz}$ input) across overdrive and classic-DSP amp channels, or per-block tiers?
4. **Optional Linux/Clang-20 RTSan CI job:** Since `dsp-core/` and `engine/` have no OS or JUCE dependencies, should DevOps add a fast secondary `ubuntu-latest` + Clang 20 `-fsanitize=realtime,address,undefined` job on `dsp-core` tests alongside the primary `windows-latest` MSVC build?

---

## 5. Sources

- ITU-R BS.1770-4, *Algorithms to measure audio programme loudness and true-peak audio level*: https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en (accessed 2026-10-25)
- EBU Tech 3341, *Loudness Metering: 'EBU Mode' metering to supplement loudness normalisation in accordance with EBU R 128*: https://tech.ebu.ch/publications/tech3341 (accessed 2026-10-25)
- `jiixyj/libebur128` (MIT License): https://github.com/jiixyj/libebur128 (accessed 2026-10-25)
- `csteinmetz1/pyloudnorm` (MIT License): https://github.com/csteinmetz1/pyloudnorm (accessed 2026-10-25)
- `csteinmetz1/auraloss` (Apache-2.0 License, Multi-Resolution STFT & spectral losses): https://github.com/csteinmetz1/auraloss (accessed 2026-10-25)
- Log-Spectral Distance overview: https://en.wikipedia.org/wiki/Log-spectral_distance (accessed 2026-10-25)
- Angelo Farina (2000), *Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique*, AES 108th Convention Paper 5093: https://www.aes.org/e-lib/browse.cfm?elib=10211 (accessed 2026-10-25)
- Audio Precision, *More About THD+N*: https://www.ap.com/technical-library/more-about-thdn/ (accessed 2026-10-25)
- F. J. Harris (1978), *On the use of windows for harmonic analysis with the discrete Fourier transform*, Proc. IEEE: https://ieeexplore.ieee.org/document/1455106 (accessed 2026-10-25)
- LLVM/Clang 20 `RealtimeSanitizer` documentation: https://clang.llvm.org/docs/RealtimeSanitizer.html (accessed 2026-10-25)
- `realtime-sanitizer/rtsan` repository & platform support: https://github.com/realtime-sanitizer/rtsan (accessed 2026-10-25)
- Microsoft Learn, `_CrtSetAllocHook` CRT Debug Library reference: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/crtsetallochook (accessed 2026-10-25)
- Microsoft Learn, `SetThreadAffinityMask` Win32 API reference: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask (accessed 2026-10-25)
- cppreference, `operator new` replacement: https://en.cppreference.com/w/cpp/memory/new/operator_new (accessed 2026-10-25)
- cppreference, `std::fpclassify`: https://en.cppreference.com/w/cpp/numeric/math/fpclassify (accessed 2026-10-25)
- `Tracktion/pluginval` repository & CLI documentation (GPLv3): https://github.com/Tracktion/pluginval and https://github.com/Tracktion/pluginval/blob/develop/docs/Testing.md (accessed 2026-10-25)
- `martinus/nanobench` (MIT License): https://github.com/martinus/nanobench (accessed 2026-10-25)
- `CatchOrg/Catch2` (BSL-1.0 License): https://github.com/CatchOrg/Catch2 (accessed 2026-10-25)
- `google/benchmark` (Apache-2.0 License): https://github.com/google/benchmark (accessed 2026-10-25)
