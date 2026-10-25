# Research: Measurement and Test Tooling for Audio DSP

- **Issue:** #13
- **Author:** `gemini/researcher` (Corvid)
- **Date:** 2026-10-25

---

## 1. Summary

1. **LUFS (ITU-R BS.1770-4 / EBU R128):** Use **`jiixyj/libebur128`** (`v1.2.6`, C, **MIT**, AGPLv3-compatible) inside C++ tools (`osp-render` / Catch2 tests) and **`csteinmetz1/pyloudnorm`** (`v0.1.1`, Python, **MIT**) in offline Python analysis scripts. Both implement ITU-R BS.1770-4 K-weighting and gated integration; self-implementing is unnecessary except as a 2-biquad verification reference.
2. **Golden-render regression:** Use a **two-tier gate** in `ctest`:
   - **Tier 1 (same-platform deterministic CI):** Sample-accurate **null test** after `getLatencySamples()` alignment; require RMS residual $\le -120\text{ dBFS}$ (peak $\le -110\text{ dBFS}$) for linear/classic-DSP blocks and bypass paths.
   - **Tier 2 (cross-compiler / SIMD / fast-math / neural blocks):** **Log-Spectral Distance (LSD)** with **mean $\le 0.2\text{ dB}$** (max $\le 1.0\text{ dB}$, silence floor $\epsilon = 10^{-10}$) plus **Multi-Resolution STFT (MR-STFT)** spectral convergence ($\le 10^{-3}$, reference implementation: `csteinmetz1/auraloss`, **Apache-2.0**). Note: Phase 6 preset tone-target matching uses a separate, looser tolerance ($\le 1.5\text{ dB}$ RMS error per octave band).
3. **THD, THD+N, and aliasing:** Combine **stepped sine + 4-term Blackman-Harris FFT** (coherent bin frequencies at $997\text{ Hz}$ for THD/THD+N and $5\text{ kHz}$–$15\text{ kHz}$ for folded aliasing below fundamental) with **Farina Exponential Sine Sweep (ESS)** deconvolution (AES 2000) to capture continuous frequency response and harmonic impulse orders $H_2 \dots H_5$ in one pass.
4. **Real-time safety on Windows/MSVC:** Clang 20's **RealtimeSanitizer (`-fsanitize=realtime` / RTSan)** supports Linux, macOS, and iOS only—**Windows is unsupported** as of LLVM 20. On Windows/MSVC, enforce RT safety via a **global `operator new` / `operator delete` override** paired with a `thread_local` audio-thread guard, supplemented in Debug builds by MSVC **`_CrtSetAllocHook`** (catches C `malloc`/`calloc`/`realloc` inside third-party libs), custom lock-wrapper assertions, and denormal checks (`_MM_GET_FLUSH_ZERO_MODE`, `_MM_GET_DAZ_MODE`, plus `std::fpclassify` output buffer assertions).
5. **Plugin validation (`pluginval`):** Fetch the pinned Windows binary **v1.0.4** (`https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip`, **GPLv3**) in CI. While `AGENTS.md` §4 requires strictness $\ge 8$ (which adds discrete parameter state restoration and *Process called with a larger than prepared block size*), we recommend running **strictness level 9 or 10** because **level 9 adds `Allocations during process`**, complementing our in-tree RT checker.
6. **Benchmarking & CI noise normalization:** Use **`martinus/nanobench`** (`v4.6.0`, single-header C++, **MIT**) for `osp-bench`. GitHub Actions `windows-latest` shared VMs exhibit $15\%$–$30\%$ wall-clock jitter (*unverified* exact runner variance; mitigated by normalization): normalize every block/preset benchmark against a pinned **reference DSP kernel** (fixed 64-biquad cascade run in the same process) in addition to reporting real-time CPU budget % ($\text{wall time} / (N / f_s) < 25\%$ at $48\text{ kHz}$ / 64 samples).

---

## 2. Findings

### 2.1 LUFS Loudness Measurement (ITU-R BS.1770-4 / EBU R128)

ITU-R BS.1770-4 defines integrated loudness (LKFS/LUFS) via a two-stage **K-weighting filter** (Stage 1: high-shelf head-acoustics filter $+4\text{ dB}$ above $\sim 1.5\text{ kHz}$; Stage 2: RLB 2nd-order high-pass at $\sim 38\text{ Hz}$), $400\text{ ms}$ rectangular gating blocks ($75\%$ overlap, $100\text{ ms}$ step), an absolute gate at $-70\text{ LUFS}$, and a relative gate at $-10\text{ LU}$ below the ungated mean ([ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en), accessed 2026-10-25; [EBU Tech 3341](https://tech.ebu.ch/publications/tech3341), accessed 2026-10-25).

| Option | Language | Version / Pin | License | AGPLv3-Compatible? | Pros / Cons | Source |
|---|---|---|---|---|---|---|
| **`jiixyj/libebur128`** | C (C99) | `v1.2.6` (verified via GitHub API) | **MIT** | Yes | Single small C library (~2 files); supports any sample rate (interpolated filter coefficients + true-peak oversampling); zero external dependencies; battle-tested in FFmpeg/Ardour. | [github.com/jiixyj/libebur128](https://github.com/jiixyj/libebur128) (2026-10-25) |
| **`csteinmetz1/pyloudnorm`** | Python | `v0.1.1` (PyPI / GitHub) | **MIT** | Yes | Clean NumPy/SciPy implementation of ITU-R BS.1770-4; ideal for `tools/` preset loudness linters. | [github.com/csteinmetz1/pyloudnorm](https://github.com/csteinmetz1/pyloudnorm) (2026-10-25) |
| **Custom C++20 implementation** | C++20 | In-tree | AGPL-3.0-or-later | Yes | ~120 LOC (2 biquads + ring buffer for $400\text{ ms}$ blocks + two-pass gating). ITU paper gives exact $48\text{ kHz}$ coefficients; other rates require analytical/bilinear design. | [ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en) (2026-10-25) |

---

### 2.2 Golden-Render Regression: Null Tests & Spectral Distance Metrics

Audio DSP golden-render regression must distinguish unintended bugs from benign floating-point reassociation across compiler versions or SIMD widths:

| Metric | Mathematical Definition | Target Tolerance | Best Use Case | Source |
|---|---|---|---|---|
| **Sample-aligned Null Test** | $E_{\text{null}} = 10\log_{10}\!\left(\frac{1}{N}\sum_{n} (y[n] - y_{\text{ref}}[n])^2\right)$ and peak $\max_n |y[n]-y_{\text{ref}}[n]|$ | RMS $\le -120\text{ dBFS}$, Peak $\le -110\text{ dBFS}$ (same OS/compiler); $\le -90\text{ dBFS}$ (cross-SIMD) | Bypass verification (`AGENTS.md` §4), deterministic regression on `windows-latest` MSVC. | Industry practice; *unverified* universal standard threshold (`AGENTS.md` specifies $\le -120\text{ dBFS}$ for bypass) |
| **Log-Spectral Distance (LSD)** | $\text{LSD} = \frac{1}{T}\sum_{t=1}^{T}\sqrt{\frac{1}{K}\sum_{k=1}^{K}\left[10\log_{10}\frac{|Y(t,k)|^2 + \epsilon}{|Y_{\text{ref}}(t,k)|^2 + \epsilon}\right]^2}$ ($\epsilon = 10^{-10}$, $-100\text{ dBFS}$ floor) | **Golden regression:** Mean $\le 0.2\text{ dB}$, Max $\le 1.0\text{ dB}$.<br>**Tone-target matching:** $\le 1.5\text{ dB}$ RMS per octave band. | Insensitive to tiny phase/rounding shifts; catches filter cutoff, EQ, or gain regressions. | [Gray & Markel 1976 / Wikipedia LSD](https://en.wikipedia.org/wiki/Log-spectral_distance) (2026-10-25) |
| **Multi-Resolution STFT (MR-STFT)** | Sum of spectral convergence $\frac{\|\,|Y|-|Y_{\text{ref}}|\,\|_F}{\|\,|Y_{\text{ref}}|\,\|_F}$ + log-magnitude $L_1$ across FFT sizes $\{512, 1024, 2048\}$ | Spectral convergence $\le 10^{-3}$ for golden regression | Captures both transient timing (short FFT) and steady-state harmonic accuracy (long FFT). Reference: `csteinmetz1/auraloss` (**Apache-2.0**). | [github.com/csteinmetz1/auraloss](https://github.com/csteinmetz1/auraloss) (2026-10-25) |

**Tolerance-setting practice:** Always silence-floor clamp magnitudes at $\epsilon = 10^{-10}$ ($-100\text{ dBFS}$) before taking logarithms so quiet noise-gate tails do not cause division-by-zero spikes in LSD. Align latency via `getLatencySamples()` before computing time-domain residuals.

---

### 2.3 THD, THD+N, and Aliasing Measurement Methods

1. **Coherent Stepped/Single-Tone FFT (with 4-term Blackman-Harris window):**
   - Choose test frequency $f_0 = \frac{m}{N_{\text{FFT}}} f_s$ (with odd integer $m$, e.g. near $997\text{ Hz}$, $N_{\text{FFT}} = 65536$) or apply a **4-term Blackman-Harris window** (sidelobe floor $-92\text{ dB}$) to suppress spectral leakage ([Audio Precision THD+N note](https://www.ap.com/technical-library/more-about-thdn/), accessed 2026-10-25; [Harris 1978 window paper](https://ieeexplore.ieee.org/document/1455106), accessed 2026-10-25).
   - **THD:** $\sqrt{\sum_{k=2}^{H} A_k^2} / A_1$ ($H=10$, or harmonics up to $f_s/2$).
   - **THD+N:** Total RMS energy from $20\text{ Hz}$ to $20\text{ kHz}$ with the fundamental bin ($f_0 \pm 3$ bins) notched out, divided by total signal RMS.
   - **Aliasing (Signal-to-Aliasing Ratio / SAR):** Drive nonlinear block (overdrive, amp) with a high-frequency tone ($f_0 = 5432\text{ Hz}$ or stepped $2\text{ kHz}$–$15\text{ kHz}$ at $-6\text{ dBFS}$). Any non-harmonic bin $f \neq k f_0$ inside $20\text{ Hz}$–$20\text{ kHz}$ (especially bins *below* $f_0$, which can only be folded aliases $|m f_s - k f_0|$) is summed as aliasing power $P_{\text{alias}}$. Oversampling verification asserts $\text{SAR} = 10\log_{10}(P_1 / P_{\text{alias}}) \ge 70\text{ dB}$ (*unverified* project threshold; to be fixed in `docs/spec/`).
2. **Farina Exponential Sine Sweep (ESS):**
   - Stimulus: $x(t) = \sin\!\left[\frac{2\pi f_1 T}{\ln(f_2/f_1)}\left(e^{\frac{t}{T}\ln(f_2/f_1)} - 1\right)\right]$ from $f_1 = 20\text{ Hz}$ to $f_2 = 20\text{ kHz}$ over $T = 2\text{ s}$ ([Farina, AES 108th Convention Paper 5093, 2000](https://www.aes.org/e-lib/browse.cfm?elib=10211), accessed 2026-10-25).
   - Convolving the output $y(t)$ with the time-reversed, $-6\text{ dB/oct}$ amplitude-weighted inverse filter separates the linear impulse response at $t = T$ from the $k$-th harmonic distortion impulse responses at negative time offsets $\Delta t_k = -T \frac{\ln k}{\ln(f_2/f_1)}$. Windowing each $\Delta t_k$ yields continuous frequency-dependent $H_2(f), H_3(f), \dots$ curves in one render.

---

### 2.4 Real-Time Safety Checkers on Windows / MSVC

| Mechanism | Windows / MSVC Support? | What It Catches | Details & Source |
|---|---|---|---|
| **Clang `RealtimeSanitizer` (`-fsanitize=realtime`, RTSan)** | **No** (Linux, macOS, iOS only as of Clang 20) | `malloc`/`free`, `pthread_mutex_lock`, syscalls, file I/O in `[[clang::nonblocking]]` functions | Upstreamed in LLVM/Clang 20, but `compiler-rt` interceptors do not support Windows/MSVC ([Clang 20 RealtimeSanitizer Docs](https://clang.llvm.org/docs/RealtimeSanitizer.html), accessed 2026-10-25; [realtime-sanitizer/rtsan](https://github.com/realtime-sanitizer/rtsan), accessed 2026-10-25). Optional secondary Linux/macOS CI job only. |
| **Global `operator new` / `delete` override + `thread_local` guard** | **Yes** (MSVC, Clang, GCC; Debug & Release-with-tests) | All C++ heap allocations (`new`, `std::vector`, `std::function` heap fallback) on the audio thread | Define `thread_local bool g_inAudioThread = false;` via RAII `ScopedNoAlloc` in `process()` / `processBlock()`. Overridden `::operator new(size_t)` increments an atomic violation counter or fails the Catch2 test if `g_inAudioThread` is true. Standard C++20 ([cppreference `operator new`](https://en.cppreference.com/w/cpp/memory/new/operator_new), accessed 2026-10-25). |
| **MSVC `_CrtSetAllocHook`** | **Yes** (MSVC Debug builds `_DEBUG`) | All CRT heap calls (`malloc`, `calloc`, `realloc`, `free`, `new`, `delete`), including inside C libraries | `<crtdbg.h>` hook `int __cdecl AllocHook(int allocType, void*, size_t, int blockType, long, const unsigned char*, int)`. Ignore `_CRT_BLOCK`, flag `_HOOK_ALLOC`/`_HOOK_REALLOC`/`_HOOK_FREE` when `g_inAudioThread` is true ([Microsoft Learn `_CrtSetAllocHook`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/crtsetallochook), accessed 2026-10-25). |
| **Lock Detection Wrapper** | **Yes** (portable C++20) | Blocking mutex acquisitions on audio thread | Wrap non-RT mutexes or route audio-thread sync exclusively through `try_lock()` / lock-free SPSC queues; assert `!g_inAudioThread` in blocking lock wrappers. |
| **Denormal Detection & FTZ/DAZ Verification** | **Yes** (`<immintrin.h>` + `<cmath>`) | Subnormal float CPU spikes ($\sim 10\times$–$100\times$ slowdown on x86) and missing FTZ/DAZ guards | 1) Assert `_MM_GET_FLUSH_ZERO_MODE() == _MM_FLUSH_ZERO_ON` and `_MM_GET_DAZ_MODE() == _MM_DENORMALS_ZERO_ON` inside `process()`. 2) Feed $0.5\text{ s}$ impulse followed by $10\text{ s}$ of silence (or $10^{-38}$ noise) and scan every output sample with `std::fpclassify(x) != FP_SUBNORMAL && std::isfinite(x)` ([cppreference `fpclassify`](https://en.cppreference.com/w/cpp/numeric/math/fpclassify), accessed 2026-10-25). |

---

### 2.5 `pluginval` in Windows CI & Known JUCE Pitfalls

- **Repository, Version & License:** [`Tracktion/pluginval`](https://github.com/Tracktion/pluginval) (latest release **`v1.0.4`**, verified via GitHub Releases API 2026-10-25), licensed under **GPLv3** (standalone validation CLI tool; invoked as an external process in CI, so no licensing impact on `dsp-core` or binary distribution).
- **Windows CI Download & Invocation (pinned to `v1.0.4` for reproducibility):**
  ```powershell
  Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip" -OutFile pluginval.zip
  Expand-Archive -Path pluginval.zip -DestinationPath tools/pluginval
  ./tools/pluginval/pluginval.exe --strictness-level 9 --validate-in-process --timeout-ms 60000 --output-dir build/pluginval-logs --validate "build/release/app/OpenSourcePedal_artefacts/Release/VST3/OpenSourcePedal.vst3"
  ```
- **What Each Strictness Level Enforces (verified against `Tracktion/pluginval@develop` `source/tests/BasicTests.cpp`, `ExtremeTests.cpp`, `EditorTests.cpp`, `ParameterFuzzTests.cpp`, accessed 2026-10-25):**
  - **Levels 1–5:** Plugin info (1), Editor & Audio processing across $44.1/48/96\text{ kHz}$ and block sizes $64 \dots 2048$ (2), Plugin state (2), Automation & Open/close editor during processing (3), Automatable parameters (4), Bus layouts (5).
  - **Level 6:** **Plugin state restoration** (`getStateInformation` / `setStateInformation`), **Fuzz parameters**, **Non-releasing audio processing**, **Editor stress**.
  - **Level 7:** **Parameters**, **Background thread state**, **Parameter thread safety** (concurrent parameter changes across threads).
  - **Level 8 (`AGENTS.md` minimum gate):** **Discrete parameter state restoration** and **Process called with a larger than prepared block size** (calls `processBlock` with up to $2 \times \text{maxBlockSize}$).
  - **Level 9 (recommended CI gate):** **Allocations during process** (`pluginval`'s own audio-thread allocator interceptor during `processBlock`), complementing our in-tree RT checker.
  - **Level 10:** Maximum iteration/fuzz stress across all registered tests.
- **Known JUCE `AudioProcessor` Pitfalls Exposed by `pluginval` (Levels 6–9):**
  1. **`processBlock` called with larger-than-prepared block size (Level 8):** `pluginval` deliberately passes buffers larger than `prepareToPlay`'s `maximumExpectedSamplesPerBlock`. The `AudioProcessor` wrapper **must** split oversized blocks into sub-chunks of $\le \text{maxBlockSize}$ before calling `engine` / `dsp-core` (which preallocate for `maxBlockSize`), rather than reallocating or overflowing on the audio thread.
  2. **Discrete parameter state restoration mismatch (Level 8):** `AudioParameterChoice` / `AudioParameterBool` values can fail restoration checks if normalized float conversion and state deserialization do not snap to identical discrete steps.
  3. **Background thread state & parameter thread-safety races (Level 7):** Calling `getStateInformation` / `setStateInformation` or mutating parameters from background threads while `processBlock` is running causes data races if state access or parameter listeners take blocking locks or touch non-atomic DSP state directly.
  4. **Dangling `AudioProcessorValueTreeState` (APVTS) listeners (Level 6 Editor stress):** Rapidly opening and closing the `AudioProcessorEditor` while parameters automate crashes if `removeParameterListener` is missed in component destructors.
  5. **Audio-thread heap allocations (Level 9):** Accidental `juce::String`, `std::vector` resize, or model deallocation inside `processBlock` trips Level 9's allocation detector.

---

### 2.6 Benchmark Harnesses & CI Runner Noise Normalization

| Library | Version / Pin | License | AGPLv3-Compatible? | Overhead / Features | Verdict | Source |
|---|---|---|---|---|---|---|
| **`martinus/nanobench`** | `v4.6.0` (verified via GitHub API) | **MIT** | Yes | Single header (`nanobench.h`); fast auto-tuning; median & median-absolute-deviation (MAD) outlier detection; JSON/CSV/Markdown output. | **Recommended** for `osp-bench` | [github.com/martinus/nanobench](https://github.com/martinus/nanobench) (2026-10-25) |
| **`CatchOrg/Catch2` (`BENCHMARK`)** | `v3.16.1` (verified via GitHub API) | **BSL-1.0** | Yes | Built into Catch2 (already planned for unit tests); convenient for quick inline checks, but slower calibration and no built-in stability warnings. | Secondary (inline sanity checks only) | [github.com/CatchOrg/Catch2](https://github.com/CatchOrg/Catch2) (2026-10-25) |
| **`google/benchmark`** | `v1.9.1` (*unverified* latest tag) | **Apache-2.0** | Yes | Industry standard, rich counters, Python `compare.py`, but heavier CMake build/link dependency than `nanobench`. | Viable alternative | [github.com/google/benchmark](https://github.com/google/benchmark) (2026-10-25) |

**Normalizing CI Runner Noise (`windows-latest`):**
Shared cloud VMs suffer from hypervisor scheduling jitter and turbo-boost frequency scaling. To prevent flaky $>10\%$ regression failures (`AGENTS.md` §4):
1. **Reference DSP Baseline Normalization:** At the start of `osp-bench`, run a pinned, deterministic reference workload $T_{\text{ref}}$: 64 cascaded RBJ biquad filters processing $48\,000$ samples ($1\text{ s}$ at $48\text{ kHz}$ in 64-sample blocks). Store each block's budget in `tests/bench/budget.json` both as **real-time CPU %** ($\text{CPU\%} = 100 \times T_{\text{block}} / T_{\text{audio\_duration}} < 25\%$) and as a **normalized ratio** $R = T_{\text{block}} / T_{\text{ref}}$. Gate CI regressions ($>10\%$) on the median of $R$ across $\ge 21$ epochs.
2. **Process Pinning on Windows:** Call `SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)` and `SetThreadAffinityMask(GetCurrentThread(), 1ULL)` inside `osp-bench` startup to minimize core-migration jitter ([Microsoft Learn `SetThreadAffinityMask`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask), accessed 2026-10-25).

---

## 3. Recommendations for OpenSourcePedal

| Category | Recommended Tool / Library | Version / Pin (verified 2026-10-25) | License | Where Used | Concrete Parameters / Thresholds |
|---|---|---|---|---|---|
| **C++ Unit & RT Tests** | `CatchOrg/Catch2` | `v3.16.1` | BSL-1.0 | `tests/` | Sample rates $\{44.1, 48, 88.2, 96\}\text{ kHz}$ $\times$ block sizes $\{0, 1, 32, 64, 127, 512, 2048\}$ plus $> \text{maxBlockSize}$ ($2\times$) at the `app/` boundary. |
| **LUFS (C++ & CLI)** | `jiixyj/libebur128` | `v1.2.6` | MIT | `tools/osp-render`, `tests/` | Integrated loudness target $-18.0\text{ LUFS-I} \pm 0.5\text{ LU}$; True Peak $\le -1.0\text{ dBTP}$. |
| **LUFS & Spectrum (Python)** | `csteinmetz1/pyloudnorm` + `scipy.signal` | `v0.1.1` | MIT / BSD-3 | `tools/analyze.py` | 1/3-octave & full-octave RMS error ($\le 1.5\text{ dB}$ for tone targets), LSD ($\epsilon = 10^{-10}$), MR-STFT (`{512, 1024, 2048}`). |
| **Golden Regression** | Built-in `osp-render` + Catch2 golden runner | In-tree | AGPL-3.0-or-later | `tests/golden/` | Null test residual $\le -120\text{ dBFS}$ (bypass & deterministic blocks); LSD mean $\le 0.2\text{ dB}$, max $\le 1.0\text{ dB}$ (cross-platform/neural). |
| **THD / Aliasing** | In-tree C++ FFT (or KissFFT / PFFFT) + Farina ESS | In-tree | BSD-3 / AGPLv3 | `tests/dsp/` | $997\text{ Hz}$ stepped tone (THD), $5432\text{ Hz}$ tone (aliasing SAR), $2\text{ s}$ $20\text{ Hz}$–$20\text{ kHz}$ ESS sweep. |
| **RT Safety Checker** | Global `operator new` override + `_CrtSetAllocHook` + FTZ/DAZ & `std::fpclassify` | In-tree (`tests/rt_checker.hpp`) | AGPL-3.0-or-later | `tests/` (Debug & Release) | 0 allocations, 0 frees, 0 blocking locks, 0 `FP_SUBNORMAL` / `FP_NAN` / `FP_INFINITE` samples in `process()`. |
| **Benchmarks** | `martinus/nanobench` | `v4.6.0` | MIT | `tests/osp-bench` | $\ge 21$ epochs, median normalized to 64-biquad reference kernel $T_{\text{ref}}$; full chain $< 25\%$ core at $48\text{ kHz}$ / 64 samples; fail CI on $>10\%$ normalized regression. |
| **Plugin Validation** | `Tracktion/pluginval` | `v1.0.4` (pinned release asset URL) | GPLv3 (external CLI) | `.github/workflows/ci.yml` | `pluginval.exe --strictness-level 9 --validate-in-process --timeout-ms 60000 --validate <vst3>` (satisfies `AGENTS.md` $\ge 8$ and enables Level 9 allocation test). |

---

## 4. Open Questions for the Architect

1. **`pluginval` CI strictness level (8 vs 9/10):** `AGENTS.md` §4 requires `--strictness-level 8` or higher. Since `pluginval`'s built-in `Allocations during process` test activates at **level 9**, should `docs/spec/realtime-rules.md` and CI standardize on `--strictness-level 9` (or `10`) from Phase 3 onward? *(Recommendation: yes, run `--strictness-level 9` in CI).*
2. **In-tree FFT for C++ unit tests (`tests/dsp/`):** Should `tests/` use a tiny permissive single-file FFT (e.g. `pffft` BSD-like or `kissfft` BSD-3-Clause) shared with `dsp-core` (for partitioned IR convolution in Phase 4/5), or keep frequency-response/THD FFT evaluation in Python (`scipy`) vs C++ Catch2? *(Recommendation: share the same BSD/MIT C++ FFT chosen for the cab IR loader so `ctest` is 100% self-contained in C++).*
3. **Golden render storage format:** Store golden reference renders in `tests/golden/` (via Git LFS) as 32-bit float `.wav` (exact bit-level null testing) or 24-bit `.flac` (smaller LFS footprint, $-144\text{ dBFS}$ quantization floor)? *(Recommendation: 32-bit float `.wav` for short 2–3 s test clips in Git LFS to avoid quantization floor masking $-120\text{ dBFS}$ null tests).*
4. **Aliasing (SAR) pass/fail threshold per nonlinear block:** Should `docs/spec/requirements.md` mandate a uniform Signal-to-Aliasing Ratio (e.g. $\ge 60\text{ dB}$ at $-6\text{ dBFS}$, $5.4\text{ kHz}$ input) across overdrive and classic-DSP amp channels, or per-block tiers?
5. **Optional Linux/Clang-20 RTSan CI job:** Since `dsp-core/` and `engine/` have no OS or JUCE dependencies, should DevOps add a fast secondary `ubuntu-latest` + Clang 20 `-fsanitize=realtime,address,undefined` job on `dsp-core` tests alongside the primary `windows-latest` MSVC build?

---

## 5. Sources

- ITU-R BS.1770-4, *Algorithms to measure audio programme loudness and true-peak audio level*: https://www.itu.int/rec/R-REC-BS.1770-4-201510-I/en (accessed 2026-10-25)
- EBU Tech 3341, *Loudness Metering: 'EBU Mode' metering to supplement loudness normalisation in accordance with EBU R 128*: https://tech.ebu.ch/publications/tech3341 (accessed 2026-10-25)
- `jiixyj/libebur128` (`v1.2.6`, MIT License): https://github.com/jiixyj/libebur128 (accessed 2026-10-25)
- `csteinmetz1/pyloudnorm` (`v0.1.1`, MIT License): https://github.com/csteinmetz1/pyloudnorm (accessed 2026-10-25)
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
- `Tracktion/pluginval` repository (`v1.0.4`, GPLv3), CLI & test docs: https://github.com/Tracktion/pluginval , https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md , https://github.com/Tracktion/pluginval/blob/develop/docs/Adding%20pluginval%20to%20CI.md , and https://github.com/Tracktion/pluginval/blob/develop/docs/Command%20line%20options.md (accessed 2026-10-25)
- `martinus/nanobench` (`v4.6.0`, MIT License): https://github.com/martinus/nanobench (accessed 2026-10-25)
- `CatchOrg/Catch2` (`v3.16.1`, BSL-1.0 License): https://github.com/CatchOrg/Catch2 (accessed 2026-10-25)
- `google/benchmark` (Apache-2.0 License): https://github.com/google/benchmark (accessed 2026-10-25)
