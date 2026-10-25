# ADR-0006: Build system, dependencies and repository layout (CMake)

- Status: **Accepted** (2026-10-25; cross-account review approved and merged in Phase 2).
- Deciders: architect (acct3/Moth). Issue #28.
- Rests on: ADR-0001 (as amended by PR #30), `docs/research/framework-comparison.md`, `asio-low-latency.md`,
  `nam-runtime.md`, `test-tooling.md`, and the oversampling note (PR #19, not yet merged). Pins and licences were
  re-checked on 2026-10-25 through the GitHub API (tag → commit SHA) unless stated otherwise.

## Context

Phase 3 starts writing code. Before that we need one answer to: how we fetch third-party code, which versions, under
which licence, how the targets map to the module boundaries in AGENTS.md §2, and which compiler settings apply. Constraints:

- `dsp-core/` is pure C++20 with no JUCE and no OS headers (ADR-0001). `engine/` has no JUCE in its public headers.
- Nothing we may not redistribute is committed. Large binaries go to Git LFS (AGENTS.md rule 3).
- Every dependency is AGPLv3-compatible and listed in `THIRD_PARTY.md` (rule 9).
- The AGENTS.md §3 commands are fixed: `cmake --preset windows-msvc`, `cmake --build --preset release`,
  `ctest --preset release`, `build/release/tools/osp-render`, `build/release/tests/osp-bench`, and the VST3 at
  `build/release/app/OpenSourcePedal_artefacts/Release/VST3/`.
- Golden renders must be deterministic on the CI compiler (`test-tooling.md` §2.2: null ≤ −120 dBFS same-platform).

## Decision

### D1. Dependency mechanism: CMake `FetchContent`, pinned by immutable hash

- Use only CMake's built-in `FetchContent`, no CPM and no vcpkg/Conan, so there is no extra tool and no extra licence.
  All declarations live in **one file**, `cmake/OspDependencies.cmake`, so a pin bump is a one-file PR.
- Git sources are pinned by **full 40-hex commit SHA** in `GIT_TAG`, with the human tag in a comment. Git verifies the
  object by its SHA. `GIT_SHALLOW` is off, because a shallow fetch cannot target a bare SHA. CI caches
  `FETCHCONTENT_BASE_DIR` keyed on the hash of `cmake/OspDependencies.cmake`.
- Non-git sources (zip/single file) are pinned with `URL` + `URL_HASH SHA256=…`.
- Offline and local overrides use the standard `FETCHCONTENT_SOURCE_DIR_<NAME>` and `FETCHCONTENT_FULLY_DISCONNECTED`.
- Third-party targets build with their own warning levels and are marked `SYSTEM` (CMake ≥ 3.25), so our `/W4 /WX`
  never fails on their headers.

### D2. Dependency list (v1)

| Name (FetchContent) | Pin (tag → commit / hash) | Licence (SPDX) | AGPL-compatible | Used by | Source note |
|---|---|---|---|---|---|
| JUCE | 9.0.4 → `3fb5cbc0340758596dac39f7c8de47d562b4896e` | AGPLv3 as stated in JUCE `LICENSE.md` (dual licence, AGPL option) | Yes | `app/`, `ui/` only | framework-comparison R1 |
| ASIO headers | bundled in JUCE ≥ 8.0.11 (`modules/juce_audio_devices/native/asio/`) | GPL-3.0-only (Steinberg dual licence, GPL option) | Yes (GPLv3 §13) | `app/` (`JUCE_ASIO=1`) | asio-low-latency R1 |
| VST3 SDK | bundled in JUCE | MIT | Yes | `app/` | framework-comparison §2.1 |
| NeuralAmpModelerCore | v0.6.0 → `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842` | MIT | Yes | `engine/` (private) | nam-runtime §3 |
| ↳ Eigen (NAM core submodule) | `bc3b39870ecb690a623a3f49149a358b95c5781d` (gitlab.com/libeigen/eigen) | MPL-2.0 (*verify at this commit when vendoring*) | Yes | via NAM core | nam-runtime S13 |
| ↳ AudioDSPTools (submodule) | `0827c6c2fc0deced568536142ea86f189e0b98a1` | MIT | Yes | Lanczos resampler for NAM | nam-runtime §2.5 |
| nlohmann/json | v3.12.0 → `55f93686c01528224f448c19128836e7df245f72` | MIT | Yes | `engine/` (presets); NAM core vendors its own copy | nam-runtime §2.1 |
| HIIR | 1.50, `http://ldesoras.fr/src/hiir-1.50.zip`, SHA-256 `8a13724e8d698a40f53687d40466443879f6250abd2229097f3990b76e6855a6` | WTFPL (v2, from `license.txt` + headers) | Yes (FSF: GPL-compatible) | `dsp-core/` oversampling | oversampling note (PR #19); licence checked in my review of #19 |
| Catch2 | v3.16.1 → `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | Yes | `tests/` | test-tooling §3 |
| nanobench | v4.6.0 → `b709728dba4df0356d467abfb0ba20781a8031c0` | MIT | Yes | `tests/osp-bench` | test-tooling §3 |
| libebur128 | v1.2.6 → `67b33abe1558160ed76ada1322329b0e9e058b02` | MIT | Yes | `tools/osp-render`, `tests/` | test-tooling §3 |
| dr_wav (dr_libs) | v0.14.6, commit pinned when `osp-render` lands | Unlicense OR MIT-0 (file header) | Yes | `tools/`, `tests/` WAV I/O (no JUCE in tools) | this ADR |

Not built or shipped, CI only: **pluginval v1.0.4** release binary (GPL-3.0), downloaded in the workflow (test-tooling §2.5).
Python tooling (`tools/*.py`) pins `pyloudnorm==0.1.1`, `numpy` and `scipy` in `tools/requirements.txt` (MIT/BSD).

Deliberately **not** in v1: NeuralAudio and RTNeural (nam-runtime Q1, owner: NAM ADR #26), clap-juce-extensions
(CLAP is out of v1, framework-comparison Q1), and an FFT library (see Deferred).

Rules:
- A dependency enters the build only in the PR that first uses it, together with its `THIRD_PARTY.md` row. This table is
  the approved list. Anything else needs an ADR amendment.
- Pin bumps are their own PRs (`chore(deps): …`), with goldens, benchmarks and pluginval re-run.
- `EIGEN_MPL2_ONLY` is defined for every TU that sees Eigen, so any non-MPL2 Eigen code fails to compile (*unverified*:
  macro behaviour at this commit). The NAM core Eigen alignment settings are applied as `PUBLIC` definitions on one
  target, so every TU agrees (nam-runtime §2.2 sharp edge).

### D3. Target layout

| Target | Kind | Dir | Links | Notes |
|---|---|---|---|---|
| `osp_dsp_core` | STATIC | `dsp-core/` | HIIR (header-only, PRIVATE) | C++20, no JUCE, no OS headers. Only standard headers + `<immintrin.h>` behind a `dsp-core/platform/` shim for FTZ/DAZ |
| `osp_engine` | STATIC | `engine/` | `osp_dsp_core` PUBLIC; NAM core, nlohmann_json PRIVATE | Graph, parameters, presets, NAM/IR loading, loader thread. Public headers: no JUCE, no NAM, no Eigen |
| `OpenSourcePedal` | `juce_add_plugin` | `app/` | `osp_engine`, JUCE modules | `FORMATS Standalone VST3`. Produces `OpenSourcePedal_Standalone` and `OpenSourcePedal_VST3` |
| `osp_ui` | INTERFACE (sources) | `ui/` | added to `OpenSourcePedal` | INTERFACE, so the JUCE modules compile once, in the plugin target |
| `osp_tests` | executable | `tests/` | `osp_engine`, Catch2 | Unit, golden, RT-checker tests. Registered with `catch_discover_tests` |
| `osp-bench` | executable | `tests/bench/` | `osp_engine`, nanobench | Output to `build/<preset>/tests/osp-bench` |
| `osp-render` | executable | `tools/` | `osp_engine`, libebur128, dr_wav | Offline render CLI, JUCE-free |

Layering is enforced by the link graph, which only points down (`app → engine → dsp-core`), plus one guard rule. The
release-devops ticket adds a check to `tools/guard/check_repo.py` that fails when a file under `dsp-core/` or a header
under `engine/include/` includes `juce_`, `<windows.h>` or `<Windows.h>`.

### D4. Build options

| Option | Default | Effect |
|---|---|---|
| `OSP_BUILD_APP` | ON | Fetches JUCE, builds `app/` + `ui/`. OFF gives a JUCE-free build of dsp-core/engine/tests/tools. This enables the optional Linux/Clang RTSan job (test-tooling Q5, owner #29) |
| `OSP_BUILD_TESTS` | ON | `tests/` and `osp-bench` |
| `OSP_BUILD_TOOLS` | ON | `osp-render` |
| `OSP_ENABLE_ASIO` | ON | `JUCE_ASIO=1` (headers bundled in JUCE) |
| `OSP_RT_CHECKER` | ON in Debug, ON in `osp_tests` | Compiles the allocation/lock checker (spec: `realtime-rules.md`, #25) |
| `OSP_ENABLE_AVX2` | OFF | `/arch:AVX2`. Revisit with Phase-3 NAM benchmarks (nam-runtime §2.4). Baseline x64 is SSE2 |
| `OSP_WARNINGS_AS_ERRORS` | ON in CI preset, OFF locally | `/WX` on our targets only |

### D5. Presets (`CMakePresets.json`, schema v6, CMake ≥ 3.25)

| Configure preset | Generator | binaryDir | Notes |
|---|---|---|---|
| `windows-msvc` | Ninja | `build/release` | `CMAKE_BUILD_TYPE=Release` with debug info (`/Zi`, separate PDB). Needs a VS 2022 developer shell (CI: `ilammy/msvc-dev-cmd`) |
| `windows-msvc-debug` | Ninja | `build/debug` | Debug, `OSP_RT_CHECKER=ON` |
| `windows-msvc-ci` | Ninja | `build/release` | Inherits `windows-msvc`, `OSP_WARNINGS_AS_ERRORS=ON` |
| `windows-vs` | Visual Studio 17 2022 | `build/vs` | IDE convenience only. Paths differ (multi-config), so not used by docs or CI |
| `linux-clang` | Ninja | `build/linux` | `OSP_BUILD_APP=OFF`, for the optional sanitizer job |

Build presets `release`/`debug` and test presets `release`/`debug` (output on failure, `-j` = cores) map onto these. The
AGENTS.md §3 paths hold because Ninja is single-config.

### D6. Compiler and linker settings (MSVC v143)

- `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`.
- Our targets get `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP` (+ `/WX` via option).
- **`/fp:precise`** everywhere. Never `/fp:fast`: it breaks golden determinism and NaN checks (test-tooling §2.2, §2.4).
- **Static CRT** for every target, ours and third-party: `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>`.
  The standalone and the VST3 then ship without a VC++ redistributable, and all targets agree on one CRT (mixing CRTs is a
  link error).
- Release: `/O2 /Ob2 /GL` and `/LTCG /OPT:REF /OPT:ICF`. Debug info stays on in Release, so CI crash dumps symbolise.
- Definitions: `NOMINMAX`, `WIN32_LEAN_AND_MEAN` (app only). `_USE_MATH_DEFINES` is not used; we use `std::numbers`.
- JUCE settings on `OpenSourcePedal`: `JUCE_ASIO=1`, `JUCE_WASAPI=1`, `JUCE_DIRECTSOUND=0`, `JUCE_WEB_BROWSER=0`,
  `JUCE_USE_CURL=0`, `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_STRICT_REFCOUNTEDPOINTER=1`. `COMPANY_NAME "OpenSourcePedal"`,
  `PLUGIN_MANUFACTURER_CODE Ospd`, `PLUGIN_CODE Osp1`, `IS_SYNTH FALSE`, `NEEDS_MIDI_INPUT FALSE`,
  `COPY_PLUGIN_AFTER_BUILD FALSE` (CI has no write access to the system VST3 folder).

### D7. Repository hygiene

- `.gitattributes`: `assets/** filter=lfs`, `tests/golden/**/*.wav filter=lfs`, `*.nam filter=lfs`, `*.wav filter=lfs`.
  Text files are `eol=lf`, except `*.sln`, `*.ps1` and `*.bat` (`eol=crlf`).
- `build/`, `out/` and `.vs/` are git-ignored. `FETCHCONTENT_BASE_DIR` defaults to `build/_deps`, so no third-party
  source lands in the tree, and the guard's ASIO-file check keeps passing.

## Deferred (with owner)

| Question | Owner | Note |
|---|---|---|
| FFT / partitioned convolution library for the cab IR and the test FFT (test-tooling Q2) | Cab block ticket (dsp-engineer) via an ADR-0006 amendment | Candidates: PFFFT (`marton78/pffft` v1.1.0; licence BSD-like, GitHub reports NOASSERTION, must be verified) or Signalsmith `dsp` (MIT, time-based-effects PR #32). Must be shared by `dsp-core` and `tests/` |
| NeuralAudio as a second NAM backend | NAM ADR (#26) | Not fetched in v1 |
| Linux/Clang RTSan CI job | Test strategy ADR (#29) | Enabled by `OSP_BUILD_APP=OFF` |
| pluginval strictness 9 vs 8 | #29 | Download mechanism fixed here |
| AVX2 baseline | dsp-engineer, after Phase-3 `osp-bench` | `OSP_ENABLE_AVX2` exists from day one |
| Exact `dr_wav` commit pin | First `osp-render` PR | Version 0.14.6 is the approved line |

## Consequences

- \+ One file, `cmake/OspDependencies.cmake`, holds every pin. Bumps are small, reviewable PRs. No package manager to install.
- \+ The JUCE-free build (`OSP_BUILD_APP=OFF`) keeps `dsp-core` honest, and dsp tests can run on non-Windows CI.
- \+ The static CRT and `/fp:precise` give reproducible goldens and a redistributable-free installer.
- − Non-shallow git fetch of JUCE is slow on a cold cache (size *unverified*). Mitigated by the CI cache and local overrides.
- − HIIR comes from a plain-HTTP personal site. The SHA-256 pin protects integrity, but availability is a risk. If the
  site disappears, vendor the WTFPL sources under `dsp-core/third_party/hiir/` (allowed by the licence) with a
  `THIRD_PARTY.md` row.
- − `/fp:precise` and SSE2 cost some speed compared with `/fp:fast`/AVX2. We accept that until benchmarks show a need.
