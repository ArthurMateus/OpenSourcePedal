# ADR-0007: Test strategy, measurement tooling, and CI quality gates

- **Status:** Accepted (2026-10-25)
- **Deciders:** `gemini/architect` (Corvid)
- **Inputs:** [`docs/research/test-tooling.md`](../research/test-tooling.md) (#13), [`docs/research/oversampling.md`](../research/oversampling.md) (#10), [`docs/research/nam-runtime.md`](../research/nam-runtime.md) (#6), [`docs/research/time-based-effects.md`](../research/time-based-effects.md) (#11), [`AGENTS.md`](../../AGENTS.md) §3–§4

---

## Context

Agents working on OpenSourcePedal cannot listen to audio output; every DSP correctness, tone-target, real-time safety, and performance claim must be verified by automated, deterministic measurements in CI (`AGENTS.md` §4). We need a unified architecture decision that selects the test/measurement libraries, resolves the open questions in `docs/research/test-tooling.md`, aligns the aliasing measurement harness with `docs/research/oversampling.md` (#10 / ADR-0005), defines the `tests/bench/budget.json` normalization schema for noisy cloud CI runners (`windows-latest`), and establishes numbered, testable requirements (`REQ-TEST-*`).

---

## Decision

### 1. Toolchain and Library Selection

All libraries below are AGPLv3-compatible (`ADR-0001`) and verified in `docs/research/test-tooling.md`:

| Role | Library / Tool | Version Pin | License | Target / Scope |
|---|---|---|---|---|
| **C++ unit, golden & RT tests** | `CatchOrg/Catch2` | `v3.16.1` | BSL-1.0 | `tests/` (`ctest --preset release` and `debug`) |
| **Loudness (LUFS / True Peak in C++)** | `jiixyj/libebur128` | `v1.2.6` | MIT | `tools/osp-render`, `tests/` (ITU-R BS.1770-4 / EBU R128) |
| **Loudness & spectral scripts (Python)** | `csteinmetz1/pyloudnorm` + `scipy` | `v0.1.1` | MIT / BSD-3 | `tools/analyze.py` (offline preset linting & plots) |
| **In-tree C++ FFT (answers OQ-2)** | Shared C++ FFT from `dsp-core` (per ADR-0006) | Pinned in `cmake/` | MIT / BSD-like | `tests/dsp/` (keeps `ctest` 100% self-contained in C++ without shelling out to Python) |
| **Microbenchmark harness** | `martinus/nanobench` | `v4.6.0` | MIT | `tests/osp-bench` |
| **VST3 plugin validation** | `Tracktion/pluginval` | `v1.0.4` | GPLv3 (external CLI) | `.github/workflows/ci.yml` (`pluginval_Windows.zip`) |

---

### 2. Unit-Test Matrix per DSP Block (`tests/dsp/`)

Every block in `dsp-core/` implements `prepare(sampleRate, maxBlockSize)`, `reset()`, `process(...)`, and `getLatencySamples()` and must pass the following matrix in `ctest`:

- **Sample rates $\times$ block sizes & lifecycle safety (`REQ-TEST-001`):** Every combination of $f_s \in \{44\,100, 48\,000, 88\,200, 96\,000\}\text{ Hz}$ and block sizes $N \in \{0, 1, 32, 64, 127, 512, 2048\}$ with `maxBlockSize = 2048`. Calling `process()` before `prepare()` must safely no-op or zero-fill without crashing. At the `app/` (`AudioProcessor::processBlock`) boundary, tests additionally pass oversized buffers ($N = 2 \times \text{maxBlockSize} = 4096$) to verify chunk splitting (`docs/research/test-tooling.md` §2.5 pitfall #1; `docs/research/nam-runtime.md` §3).
- **Frequency response, THD, and bin-centred aliasing harness (`REQ-TEST-002`, answers OQ-4):**
  - **Coherent bin-centred stimulus & rectangular-window FFT (`docs/research/oversampling.md` §3.3):** At $f_s \in \{44.1, 48\}\text{ kHz}$ and $N_{\text{FFT}} = 65\,536$ ($2^{16}$), pick nominal $f_0 \in \{110, 440, 1000, 2500, 5000, 8000\}\text{ Hz}$ and set bin index $k = \text{round}(f_0 N_{\text{FFT}} / f_s)$, incremented by 1 if even so $k$ is **odd** (coprime with $N_{\text{FFT}}$). Render $8\,192 + N_{\text{FFT}}$ samples at $-6\text{ dBFS}$ and $0\text{ dBFS}$, discard the first $8\,192$ warm-up samples, and compute a **rectangular-window `float64` real FFT** $X[b]$ ($b = 0 \dots N_{\text{FFT}}/2$). Because $k$ is odd, every harmonic $m \cdot k \bmod N_{\text{FFT}}$ and folded alias lands on a single exact bin with zero window leakage and no $f_s/4$ or $f_s/6$ harmonic collisions.
  - **Metrics reported by the harness:**
    1. **THD:** $\sqrt{\sum_{b \in H} |X[b]|^2} / |X[k]|$ where $H = \{m \cdot k : m \ge 2,\; m \cdot k < N_{\text{FFT}}/2\}$.
    2. **Aliasing-to-Signal Ratio (ASR):** $10\log_{10}\!\left(\sum_{b \in A} |X[b]|^2 \big/ \sum_{b \in H} |X[b]|^2\right)$ where $A = \{1 \le b < N_{\text{FFT}}/2 : b \notin \{k\} \cup H\}$.
    3. **Alias-to-fundamental:** $10\log_{10}\!\left(\sum_{b \in A} |X[b]|^2 \big/ |X[k]|^2\right)$ and worst single alias bin in $\text{dBc}$.
  - **Pass/fail thresholds:** ADR-0007 defines the measurement harness only. **Nonlinear block ASR thresholds (Live vs HQ modes and regression tolerance) are owned by ADR-0005 (#27, oversampling strategy).** For strictly linear blocks (noise gate while open, EQ, pure delay without saturation), the harness enforces a sanity gate of $\text{alias-to-fundamental} \le -80\text{ dB}$.
  - **Farina Exponential Sine Sweep (ESS):** $2\text{ s}$ log sweep ($20\text{ Hz}\text{–}20\text{ kHz}$) deconvolved with its inverse filter to extract continuous $H_1(f)$ frequency response and harmonic curves $H_2(f) \dots H_5(f)$ in one pass (`docs/research/test-tooling.md` §2.3).
- **Parameter smoothing & anti-zipper check (`REQ-TEST-003`, amended 2026-10-25 after #62):** drive the block with a bin-centred f0 ≈ 1 kHz sine and apply a **step** change of each smoothed parameter (min→max and max→min) at a non-block-aligned sample. **Gate:** the residual between the block's output and an offline **double-precision reference** that applies the parameter's specified smoothing law (parameters.md §1.4) **per sample, starting at the delivery sample** (the first block boundary at or after the step, because parameters are read once per block, RT-11) must be ≤ −60 dBc. It is RMS, normalised to the **larger** of the two settled steady-state RMS values at f0 (before the step and after the smoothing has settled). This catches zipper noise (per-block stepping, missing or wrong smoothing) and does not penalise the intended smoothing trajectory. **Informative only:** the Hann-windowed splatter outside f0 ± 50 Hz is logged. The harness (#62) showed that an *ideal* linear 50 ms +6 dB ramp already measures about −56 dBc of splatter (raised-cosine about −69 dBc), so an absolute splatter gate would fail correct implementations.
- **Bypass & reset determinism (`REQ-TEST-004`):** Hard bypass (after shifting by `getLatencySamples()`) must null against the input with **RMS residual $\le -120\text{ dBFS}$** (Delay and Reverb expose a separate `setSoftBypass(bool)` for spillover trails in the UI while keeping `setBypassed(true)` for hard null testing, per `docs/research/time-based-effects.md` §4). Calling `reset()` between two identical renders must yield bit-identical output ($-\infty\text{ dBFS}$ null).

---

### 3. Golden-Render Regression Tiers & Storage Format

- **Storage format (`REQ-TEST-005`, answers OQ-3):** Golden reference renders in `tests/golden/` are stored in **Git LFS** as **32-bit IEEE float `.wav`** files at $48\text{ kHz}$ ($2\text{–}3\text{ s}$ license-clean DI clips). Using 32-bit float `.wav` rather than 24-bit `.flac` avoids quantization noise near $-144\text{ dBFS}$ interfering with the $-120\text{ dBFS}$ Tier-1 null gate.
- **Two-tier regression gate (`REQ-TEST-005`):**
  1. **Tier 1 — Sample-aligned Null Test (same-platform deterministic paths):** After aligning by `getLatencySamples()`, assert **RMS residual $\le -120\text{ dBFS}$** and **Peak residual $\le -110\text{ dBFS}$**.
  2. **Tier 2 — Spectral Distance (cross-compiler/SIMD, neural `.nam` blocks, or intentional fast-math changes):** Compute **Log-Spectral Distance (LSD)** with silence-floor clamping $\epsilon = 10^{-10}$ ($-100\text{ dBFS}$) and **Multi-Resolution STFT (MR-STFT)** across FFT sizes $\{512, 1024, 2048\}$:
     - **Golden regression pass criteria:** Mean LSD $\le 0.2\text{ dB}$, Max 1/3-octave LSD $\le 1.0\text{ dB}$, and MR-STFT spectral convergence $\le 10^{-3}$.
     - **Phase 6 preset tone-target matching (distinct from golden regression):** Octave-band LTAS within the tolerance specified in `docs/research/tone-targets-a.md` / `tone-targets-b.md`, and integrated loudness **$-18.0\text{ LUFS-I} \pm 0.5\text{ LU}$** ($-17.0\text{ LUFS-I}$ for lead presets; True Peak $\le -1.0\text{ dBTP}$) via `libebur128`.

---

### 4. Real-Time Safety Enforcement (`tests/rt_checker.hpp` & Linux RTSan)

- **Windows / MSVC in-tree RT checker (`REQ-TEST-006`):**
  1. **C++ heap interception (Debug & Release tests):** Global replacement of `::operator new`, `::operator new[]`, `::operator delete`, and `::operator delete[]` checking `thread_local bool g_inAudioThread`. A RAII `ScopedRealtimeThread` guard wraps every `process()` / `processBlock()` call in tests and debug builds.
  2. **CRT heap interception (MSVC `_DEBUG`):** Install `_CrtSetAllocHook` (ignoring `_CRT_BLOCK`) to fail immediately if `malloc`, `calloc`, `realloc`, or `free` is invoked inside C dependencies while `g_inAudioThread` is true.
  3. **Lock & exception prohibition:** Blocking lock wrappers assert `!g_inAudioThread`; audio-thread code paths use `try_lock()` or lock-free SPSC queues (`docs/research/nam-runtime.md` §3).
  4. **Denormal (FTZ/DAZ) & non-finite sample verification:** Inside `process()` tests, assert `_MM_GET_FLUSH_ZERO_MODE() == _MM_FLUSH_ZERO_ON` and `_MM_GET_DAZ_MODE() == _MM_DENORMALS_ZERO_ON`. Feed a $0.5\text{ s}$ impulse followed by $10\text{ s}$ of silence (plus randomized parameter fuzzing across 1,000 random parameter vectors) and assert `std::fpclassify(x) != FP_SUBNORMAL && std::isfinite(x)` on every output sample.
- **Secondary Linux Clang-20 `RealtimeSanitizer` job (`REQ-TEST-006`, answers OQ-5):** Because Clang 20's `-fsanitize=realtime` (`[[clang::nonblocking]]`) does not support Windows (`docs/research/test-tooling.md` §2.4), CI will run a lightweight secondary `ubuntu-latest` + Clang 20 job on `dsp-core/` and `engine/` tests (`OSP_BUILD_APP=OFF`, per ADR-0006) compiled with `-fsanitize=realtime,address,undefined`.

---

### 5. CPU Benchmarks and CI Runner Normalization (`tests/osp-bench`)

- **Reference kernel normalization & budget policy (`REQ-TEST-007`):** To eliminate false failures from $15\%\text{–}30\%$ VM clock jitter on GitHub Actions `windows-latest`:
  1. At startup, `osp-bench` sets `HIGH_PRIORITY_CLASS` and pins thread affinity (`SetThreadAffinityMask(..., 1ULL)`).
  2. `osp-bench` first measures $T_{\text{ref}}$: a deterministic cascade of 64 RBJ biquad filters processing $48\,000$ samples ($1\text{ s}$ at $48\text{ kHz}$ in 64-sample blocks) over $\ge 21$ epochs using `nanobench`.
  3. Each block and full-preset benchmark records both **real-time CPU %** ($\text{CPU\%} = 100 \times T_{\text{median}} / T_{\text{audio\_duration}}$) and **normalized cost** $R = T_{\text{median}} / T_{\text{ref}}$ in `tests/bench/budget.json`.
  4. **Fixed architecture ceilings vs measured per-block budgets:**
     - **Fixed ceilings:** Full signal chain at $48\text{ kHz}$ / 64 samples must stay **$< 25\%$ of one core** (`AGENTS.md` §4), and the NAM amp block must stay **$\le 8\%$ of one core** (`docs/research/nam-runtime.md` §3).
     - **Per-block budgets:** Individual DSP block ceilings are **not** hardcoded from unverified research estimates; instead, each block's initial budget (`max_cpu_percent` and baseline ratio $R_0$) is recorded in `tests/bench/budget.json` from its **first measured `osp-bench` run** (plus a documented headroom margin and justification in the PR that introduces the block).
     - **CI Regression Gate:** CI fails if the full chain exceeds $25\%$, if NAM exceeds $8\%$, or if any block/preset regresses by **$> 10\%$ on normalized ratio $R$** against its stored baseline in `tests/bench/budget.json`.

---

### 6. Plugin Validation Gate (`pluginval` Strictness Level 9)

- **Strictness Level 9 in CI (`REQ-TEST-008`, answers OQ-1):** CI fetches `https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip` and runs:
  ```powershell
  pluginval.exe --strictness-level 9 --validate-in-process --timeout-ms 60000 --output-dir build/pluginval-logs --validate <path-to-OpenSourcePedal.vst3>
  ```
  Choosing **level 9** exceeds the `AGENTS.md` §4 minimum ($\ge 8$, which enforces discrete parameter state restoration and $2 \times \text{maxBlockSize}$ buffer calls) and enables `pluginval`'s built-in **`Allocations during process`** test on the compiled `.vst3` binary.

---

## Summary of Testable Requirements

| ID | Requirement Summary | Verification Target |
|---|---|---|
| `REQ-TEST-001` | Every `dsp-core` block passes unit tests across $\{44.1, 48, 88.2, 96\}\text{ kHz} \times \{0, 1, 32, 64, 127, 512, 2048\}$ samples, handles pre-`prepare()` calls safely, and `app/` splits $2 \times \text{maxBlockSize}$ buffers. | `ctest` (`tests/dsp/`, `tests/app/`) |
| `REQ-TEST-002` | Coherent odd-bin ($N_{\text{FFT}}=65536$, rectangular window) FFT harness measures THD, ASR, and alias-to-fundamental. Linear blocks achieve $\text{alias-to-fundamental} \le -80\text{ dB}$; nonlinear block ASR thresholds follow ADR-0005. ESS sweep logs $H_1 \dots H_5$. | `ctest` (`tests/dsp/`) |
| `REQ-TEST-003` | A step change of each smoothed parameter on a 1 kHz sine: the residual against the per-sample double-precision reference of the specified smoothing law (started at the delivery block boundary, normalised to the larger settled RMS) is ≤ −60 dBc. Splatter is logged, not gated. | `ctest` (`tests/dsp/`) |
| `REQ-TEST-004` | Hard bypass nulls against latency-compensated input at $\le -120\text{ dBFS}$ RMS; `reset()` produces bit-identical repeat renders. | `ctest` (`tests/dsp/`) |
| `REQ-TEST-005` | Golden renders (32-bit float `.wav` in Git LFS) pass Tier-1 null ($\le -120\text{ dBFS}$ RMS) or Tier-2 spectral gates (LSD mean $\le 0.2\text{ dB}$, MR-STFT $\le 10^{-3}$); factory presets normalize to $-18.0 \pm 0.5\text{ LUFS-I}$ (leads $-17.0$). | `ctest -L golden`, `tools/osp-render` |
| `REQ-TEST-006` | Zero heap allocations/frees (`operator new` + `_CrtSetAllocHook`), zero blocking locks, FTZ/DAZ enabled, and zero `FP_SUBNORMAL`/NaN/Inf samples in `process()`; secondary Linux Clang-20 RTSan job green. | `ctest` (`tests/rt/`), GitHub Actions |
| `REQ-TEST-007` | Full chain $< 25\%$ core and NAM $\le 8\%$ at $48\text{ kHz}$ / 64 samples; per-block budgets initialized from first measured `osp-bench` run; no $> 10\%$ regression in normalized ratio $R = T / T_{\text{ref}}$ against `tests/bench/budget.json`. | `osp-bench` in CI |
| `REQ-TEST-008` | Packaged `.vst3` passes `pluginval v1.0.4` at `--strictness-level 9 --validate-in-process` plus byte-for-byte state save/load round-trip test. | `pluginval` in CI |

---

## Consequences

- **+** Every quality gate in `AGENTS.md` §4 maps to an exact mathematical metric, tolerance, and pinned open-source library.
- **+** All five open questions from `docs/research/test-tooling.md` are resolved without deferrals, and the aliasing harness matches `docs/research/oversampling.md` §3.3 / ADR-0005 without conflicting thresholds.
- **+** Reference-kernel normalization ($R = T_{\text{block}} / T_{\text{ref}}$) and empirically initialized per-block budgets protect the $10\%$ regression gate against cloud VM clock variance while preserving the hard $25\%$ full-chain and $8\%$ NAM real-time CPU ceilings.
- **−** Storing 32-bit float `.wav` golden renders uses roughly $2\times$ the Git LFS storage of 24-bit `.flac`, which we mitigate by keeping golden DI test clips short ($2\text{–}3\text{ s}$).
