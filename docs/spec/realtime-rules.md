# Real-time rules

- Status: draft spec for ADR-0004 (issue #25). Date: 2026-10-25. Owner: architect.
- Scope: everything reachable from `PluginProcessor::processBlock`, `Engine::process()` and every `dsp-core`
  `process()`. That is the **audio thread** (threading.md §1). AGENTS.md rule 5: violations block the merge.
- Sources: `docs/research/test-tooling.md` §2.4–2.5, `asio-low-latency.md` §2.3 and R6–R9, `nam-runtime.md` §2.2.

## 1. Rules

| ID | Rule | Why / source | Enforced by |
|---|---|---|---|
| RT-1 | **No heap allocation or deallocation**: no `new`/`delete`, `malloc`/`free`, container growth, `std::string` building, `std::function` construction, `shared_ptr` creation or last-reference release, or `juce::String`/`var` | Allocators take locks and can page-fault | C1, C2, C6 |
| RT-2 | **No blocking synchronisation**: no `std::mutex`, `std::condition_variable`, `std::atomic::wait/notify`, critical sections, `juce::CriticalSection`, `SpinLock::enter`, or semaphores | Priority inversion | C3, C6 |
| RT-3 | **No system calls**: no file/console/network I/O, `OutputDebugString`, `Sleep`, `SetEvent`, thread creation, `VirtualAlloc`/`VirtualLock`, registry, COM, MMCSS calls, or `timeBeginPeriod`. **Single exception:** `QueryPerformanceCounter` in the app's CPU meter (threading.md §7) | Unbounded kernel time; asio-low-latency R8–R9 | C6, review |
| RT-4 | **No logging**, including `DBG`, `printf` and `std::cout`. Report through atomics (threading.md §3.5) | I/O + allocation | C6, review |
| RT-5 | **No exceptions**: audio-path functions are `noexcept`. Code that can throw (NAM `get_dsp`, `GetLoudness()` without `HasLoudness()`, JSON parsing) runs only on control or worker threads | `throw` allocates and unwinds | C5, review |
| RT-6 | **Lock-free atomics only**: `static_assert(std::atomic<T>::is_always_lock_free)` for every atomic the audio thread touches | A non-lock-free atomic hides a lock | C5 |
| RT-7 | **Bounded work**: every loop is bounded by block size, channel count, or a constant fixed at `prepare()`. No waiting or spinning on other threads | Deadline 1.33 ms at 48 kHz / 64 | Review, C7 |
| RT-8 | **Denormals flushed**: FTZ+DAZ set at callback entry (`juce::ScopedNoDenormals` in the app, `osp::ScopedFlushDenormals` in `dsp-core` tests and `osp-render`) | asio-low-latency R6; 10–100× slowdowns (test-tooling §2.4) | C4 |
| RT-9 | **Finite output**: the engine output stage replaces non-finite samples with 0 and sets `nanDetected`. Tests require zero occurrences | Speaker and ear safety; fuzz | C4, C8 |
| RT-10 | **Only `prepare()` allocates**: buffers are sized for `maxBlock` × max oversampling factor × channels. `process(n)` with `n ≤ maxBlock` must work for any n ≥ 0 | pluginval level 8; nam-runtime §3 | C8 |
| RT-11 | **No destructors that free on the audio thread**: retired objects go to the retire queue (threading.md §4) | nam-runtime §2.5 bug | C1, C2 |
| RT-12 | **Pre-touch**: after allocating in `prepare()`/worker builds, write every byte of buffers, IRs, model weights and delay lines once | Avoid first-use page faults; asio-low-latency R7 | Review, C7 (xrun counter in soak test) |
| RT-13 | **Parameter changes are smoothed**: continuous parameters ramp per sample, and discrete changes that would click get a short ramp (threading.md §3) | No zipper noise (AGENTS.md §4) | Block unit tests (step-response test) |
| RT-14 | **Never call host or JUCE APIs that may lock** from the audio thread: no `setLatencySamples`, `updateHostDisplay`, or `MessageManager` calls | Host-side locks (*unverified* per host; treat as unsafe) | Review |

Allowed: preallocated memory, `std::array`, `std::span`, fixed-capacity containers that never allocate, lock-free SPSC
queues (`engine/rt/`), relaxed or acquire/release atomics, `<cmath>` functions, virtual calls to already-built objects,
and `try_lock` on a mutex shared with a non-RT thread only where a fallback exists (avoid; needs review sign-off).

## 2. Enforcement (the RT checker)

The checker lives in a CMake object library, `osp_rt_checker` (`OSP_RT_CHECKER`, ADR-0006 D4). It is linked into
`osp_tests` and into Debug builds of the app. It is never in Release binaries.

| ID | Mechanism | Detail | Source |
|---|---|---|---|
| C1 | **Audio-thread scope** | `osp::rt::ScopedAudioThread` sets a `thread_local bool` on entry to `processBlock`, `Engine::process()`, or the test harness's call to `process()`. Nesting is allowed | test-tooling §2.4 |
| C2 | **Global `operator new`/`delete` replacement** (all 8+ forms incl. aligned and nothrow) | If the flag is set: atomic `allocViolations++`. In tests the harness fails the case. In Debug app builds, `__debugbreak()` only if a debugger is attached; the counter is shown in a debug overlay | test-tooling §2.4 |
| C2b | **MSVC `_CrtSetAllocHook`** (Debug CRT) | Catches `malloc`/`realloc`/`free` from C code and third-party libraries (e.g. Eigen's aligned malloc). Ignores `_CRT_BLOCK` | test-tooling §2.4 |
| C3 | **Lock wrapper** | All non-RT mutexes in `engine/` are `osp::rt::Mutex` (wraps `std::mutex`). `lock()` asserts the flag is clear. A guard rule (release-devops) bans a bare `std::mutex` / `std::condition_variable` outside `engine/rt/` | test-tooling §2.4 |
| C4 | **FP-state and output checks** | At `Engine::process()` entry (Debug and tests), assert `_MM_GET_FLUSH_ZERO_MODE()` / `_MM_GET_DAZ_MODE()` are on. In tests, scan every output sample with `std::isfinite` and `fpclassify != FP_SUBNORMAL` | test-tooling §2.4 |
| C5 | **Compile-time** | `noexcept` on every `process()` and audio-path function. `static_assert` lock-free atomics. Queue element types `static_assert(std::is_trivially_copyable_v<T>)` | — |
| C6 | **External and Linux checks** | pluginval at **strictness 9** (adds "Allocations during process"), as fixed by ADR-0007. A macro `OSP_RT_NONBLOCKING` expands to `[[clang::nonblocking]]` on Clang ≥ 20 (empty on MSVC), so the Linux Clang-20 RTSan job (`OSP_BUILD_APP=OFF`, ADR-0006 D4; ADR-0007) checks the same functions | test-tooling §2.4–2.5, Q1, Q5 |
| C7 | **Soak test** | 10 minutes of processing at 48 kHz / 64 with parameter fuzzing and a model/IR swap every 2 s. Requires `xrunCount == 0` on the reference machine (*unverified* on shared CI runners: report only there) | — |
| C8 | **Block-size matrix** | Every block runs at sample rates {44.1, 48, 88.2, 96} kHz and block sizes {0, 1, 32, 64, 127, 512, 2048}; the app boundary also gets 2 × maxBlock. Assert zero violations from C2/C2b/C3, finite output, and correct latency | AGENTS.md §4; test-tooling §3 |

Test-side API (sketch, `tests/rt/rt_checker.hpp`):

```cpp
osp::rt::ViolationCounters before = osp::rt::counters();
{ osp::rt::ScopedAudioThread audio; engine.process(io, n); }
REQUIRE(osp::rt::counters() == before);   // allocs, frees, locks: unchanged
```

## 3. Review checklist additions (for `/review-pr`)

- Every new audio-path function is `noexcept` and marked `OSP_RT_NONBLOCKING`.
- No container or member on the audio path can grow. Capacities are fixed in `prepare()` or the worker build.
- Heavy-object changes go through the command and retire queues (threading.md §4). Nothing new owns memory across the
  thread boundary.
- New atomics have the `is_always_lock_free` assert. New queue payloads are trivially copyable.
- Tests: the RT checker wraps every `process()` call in the block's unit tests (C1–C4, C8).
