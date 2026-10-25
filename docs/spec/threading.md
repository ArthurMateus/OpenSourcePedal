# Threading model

- Status: draft spec for ADR-0004 (issue #25). Date: 2026-10-25. Owner: architect.
- Companion: `realtime-rules.md` (what is forbidden on the audio thread and how it is enforced).

## 1. Threads

| Role | Owner | Priority | Runs | Must not |
|---|---|---|---|---|
| **Audio** | ASIO driver / JUCE WASAPI thread (MMCSS "Pro Audio") / DAW | Driver or host decides; we never change it (asio-low-latency §2.3) | `Engine::process()` via `PluginProcessor::processBlock` | Anything in realtime-rules.md |
| **Message** | JUCE | Normal | UI, 30 Hz meter timer, `setLatencySamples`, worker-completion callbacks | Touch DSP objects directly |
| **Control** | Host (any non-audio thread), often the message thread | Host | `prepareToPlay`, `releaseResources`, `get/setStateInformation`, preset/model/IR requests | Run concurrently with `prepare` on another control thread (serialised by the controller mutex) |
| **Worker** | Engine (`std::jthread`) | Normal (no MMCSS) | Model/IR loading, `Chain` building, `prepare()` of new objects, deleting retired objects, latency-measurement analysis | Push to the audio thread except through the command queue |

The worker wakes on a job (condition variable, signalled by the control side) or every **50 ms** to drain the retire
queue. The audio thread never signals it: signalling is a system call (realtime-rules.md RT-3).

## 2. Objects and ownership

```
EngineController (control side, mutex)          Engine (audio side, lock-free)
  desiredState  ── job ──► Worker ── builds ──► Command{Install, ptr, gen} ──► cmdQueue (SPSC, cap 64) ──► audio
  liveState mirror                                                    audio ──► retireQueue (SPSC, cap 128) ──► Worker deletes
  paramAtomics* (registered once)  ◄──────────── read every block ─────────────── audio
  meters / telemetry (atomics)     ◄──────────── written every block ──────────── audio
```

| Object | Created on | Lives on | Destroyed on |
|---|---|---|---|
| `Chain` (graph of dsp-core blocks for one preset + quality) | Worker | Audio (installed) | Worker (via retire queue) |
| NAM model (`IAmpModel`, nam-runtime §3) | Worker (`get_dsp` → validate → wrap → `Reset` → prewarm) | Audio | Worker |
| IR / convolver state | Worker (load, resample, partition) | Audio | Worker |
| Parameter atomics | App (JUCE APVTS) | Whole plugin lifetime | App |
| Queues, meters, telemetry | Engine constructor | Whole plugin lifetime | Engine destructor (audio stopped) |

`Chain` holds its blocks by value or in a worker-allocated arena. Installing a chain is one pointer write on the audio
thread.

**Queues.** Each queue has exactly one producer thread and one consumer thread:

| Queue | Producer | Consumer | Capacity |
|---|---|---|---|
| job queue (non-RT, mutex + condition variable) | control (`EngineController`) | worker | unbounded (the control side may allocate) |
| `cmdQueue` (SPSC) | **worker only** | audio | 64 |
| `retireQueue` (SPSC) | audio | worker | 128 ≥ cap(`cmdQueue`) + `kMaxActiveObjects` (16) |
| telemetry rings (SPSC) | audio | message | per stream, fixed |

The SPSC implementation (`engine/rt/SpscQueue.hpp`) uses a power-of-two ring with monotonic `std::atomic<uint32_t>`
`head` (written only by the producer) and `tail` (written only by the consumer). The producer writes the slot, then
does `head.store(h + 1, std::memory_order_release)`. The consumer does `head.load(std::memory_order_acquire)` before
reading the slot. Symmetrically, the consumer does `tail.store(t + 1, release)` after reading, and the producer does
`tail.load(acquire)` before reusing a slot. The release/acquire pair on `head` makes every store the worker made while
building the object happen-before the audio thread dereferences `ptr`. Payloads are trivially copyable
(realtime-rules.md C5).

`kMaxActiveObjects` = 16 bounds the heavy objects the audio side can hold at once (2 chains + 3 amp channels × 2 +
2 IR slots × 2 during fades, plus spare). The engine asserts it in debug builds.

## 3. Parameter flow

1. JUCE owns each parameter as an APVTS value (`std::atomic<float>`, normalised → plain value). At `prepare()`, the app
   registers `std::span<std::atomic<float>* const>` with the engine, indexed by the engine's parameter-id table
   (`docs/spec/parameters.md`, #23). Engine public headers stay JUCE-free (ADR-0006 D3).
2. At block start, the audio thread does a relaxed load of every registered atomic. Values are independent, so no
   ordering is needed. It then hands targets to the blocks.
3. Each `dsp-core` block smooths continuous parameters per sample. The default is a one-pole with the τ from
   parameters.md; delay time uses its own 80 ms scheme (time-based-effects §3). Discrete parameters (switches, enums)
   apply at the block boundary with a short internal ramp where a step would click.
4. Parameters that need allocation or a re-prepare (oversampling quality, NAM model, IR file, cab mic count) are
   **not** read by the audio thread. A JUCE listener on the message thread turns each change into a controller request
   (§4).
5. Engine → UI: meters (peak/RMS per I/O), CPU load, `nanDetected`, `xrunCount` and `liveLatencySamples` are
   `std::atomic<float/int>` written once per block (relaxed). The UI polls them at 30 Hz. Streams (tuner frames,
   scope) use a fixed-size SPSC ring of POD frames (audio → message). If the ring is full, frames are dropped and
   counted.

## 4. Heavy-object swap protocol

1. A control call (`loadPreset`, `setAmpModel`, `setIr`, `setQuality`) locks the controller, updates `desiredState`,
   and enqueues a worker job stamped with the current generation `g`. It returns at once.
2. The worker builds the object off-thread and `prepare()`s it with the current `(sampleRate, maxBlock)`. It pre-touches
   every buffer (realtime-rules.md RT-12). On failure (bad file, `NamFileValidationError`), it posts an error to the
   message thread and enqueues nothing.
3. The worker first **drains `retireQueue` completely** (deleting each object), then pushes
   `Command{Install, kind, ptr, g}`. If `cmdQueue` is full, it drains again and retries after 1 ms; the queue is never
   grown. Every object on the audio side either arrived through `cmdQueue` (at most 64 in flight) or is active
   (at most `kMaxActiveObjects`). Draining before each push therefore bounds `retireQueue` occupancy by
   64 + 16 ≤ 128, so the audio thread's retire push can never fail. A debug assert checks it.
4. At the next block start, the audio thread pops commands. If `g` ≠ the current generation, it pushes `ptr` straight
   to the retire queue. Otherwise it starts the **ADR-0004 crossfade** (ADR-0004 §5 owns the length): old and new both run, mixed with a linear gain ramp,
   because the two outputs are strongly correlated (*unverified*: equal-gain vs equal-power to be confirmed by listening
   in the first NAM ticket). At fade end the old object goes to the retire queue.
5. If a newer install of the same kind arrives mid-fade, the fade restarts from the current mix toward the newest
   object. The superseded one is retired. At most two instances per kind run at once.
6. When the latency changes (quality, resampler, linear-phase options), the worker includes the new latency. The
   message thread calls `setLatencySamples` when it receives the "built" callback, before the swap completes. A
   reporting mismatch of up to one fade is tolerated and documented.

## 5. prepare / release / sample-rate change

- `prepareToPlay(sr, maxBlock)` runs on a control thread while audio is stopped. This is JUCE's contract that
  `processBlock` does not overlap `prepareToPlay` (*unverified* for every host; we also guard with the generation).
  It locks the controller, bumps the generation, cancels pending worker jobs, **waits for any worker build in progress
  to finish**, then drains `cmdQueue` (retiring its objects) and `retireQueue`. Audio is stopped, so the control thread
  may consume both queues here. It then rebuilds the current `desiredState` synchronously (allocation is fine here,
  including NAM `Reset`), installs it directly, and registers the parameter atomics.
- `releaseResources` retires everything synchronously (audio is stopped).
- Host blocks larger than `maxBlock` are split by the app into chunks of at most `maxBlock` (pluginval level 8).
  `numSamples == 0` returns immediately. Odd sizes (1, 127, …) are legal.

## 6. State save/restore

Both calls take the controller mutex, so they are serialised against each other and against `prepareToPlay`.

- `getStateInformation` snapshots **the current APVTS parameter values** (reading the atomics) together with the heavy
  parts of `desiredState` (preset id, `.nam`/IR references, quality). It writes them in a canonical key order, so two
  calls with no change in between are byte-identical (REQ-902). It never reads audio-side objects.
- `setStateInformation` parses on the calling thread (exceptions are caught there). It then **synchronously writes every
  restored parameter value into the APVTS parameters** before returning, with discrete and bool values snapped exactly
  as the host's normalisation does. It updates `desiredState`. Only the heavy-object rebuild (Chain/NAM/IR) is enqueued
  to the worker. A `getValue()` or `getStateInformation` on the next line therefore sees the restored state
  (pluginval levels 6–8).
- `Engine::drainPending(timeout)` (control thread only) waits until the live state matches `desiredState`. It is used by
  `osp-render`, golden tests, and offline renders when the host sets `isNonRealtime()`.

## 7. Telemetry and diagnostics

- CPU load: the app measures wall time per callback with `QueryPerformanceCounter` (one call at entry and one at exit;
  the only timing call allowed, see realtime-rules.md RT-3) and stores the smoothed load in an atomic.
- Overruns: `xrunCount` increments when the callback took longer than `numSamples / sr`.
- Latency measurement (asio-low-latency R10, Q4): the app plays a pre-generated test signal and records into a
  preallocated capture buffer on the audio thread. The worker runs the detector (`engine/`, JUCE-free) afterwards.

## 8. Open questions resolved or deferred

| Question (source) | Resolution |
|---|---|
| Swap crossfade length, swaps during playback (nam-runtime Q5) | ADR-0004 §5 crossfade; allowed during playback |
| Where the latency detector lives (asio-low-latency Q4) | Detector in `engine/`, capture in `app/`, analysis on the worker (§7) |
| `VirtualLock` default (asio-low-latency Q5) | Off in v1. Pre-touch is mandatory (RT-12). Revisit if page-fault xruns are measured. Owner: audio-io-engineer |
| Delay/reverb tails across preset switches | Deferred to `signal-flow.md` (#22). v1 baseline: the whole chain crossfades |
| Parallel DSP threads | Not in v1. If added later, worker threads join MMCSS "Pro Audio" (asio-low-latency R9). Needs an ADR-0004 amendment |
