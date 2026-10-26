# ADR-0004: Threading model and real-time handoff

- Status: **Accepted** (2026-10-25; cross-account review approved and merged in Phase 2).
- Deciders: architect (acct3/Moth). Issue #25.
- Rests on: `docs/research/nam-runtime.md` (§1.7, §2.5, §3), `test-tooling.md` (§2.4, §2.5), `asio-low-latency.md`
  (§2.3, R6–R9), ADR-0006 (target layout). Details: `docs/spec/threading.md` and `docs/spec/realtime-rules.md`.

## Context

The audio callback runs on a thread we don't own: the ASIO driver's thread, JUCE's WASAPI thread (MMCSS "Pro Audio"),
or a DAW thread. At 48 kHz / 64 samples it has a 1.33 ms deadline (asio-low-latency §2.3). Meanwhile we must load NAM
models and IRs (whose load functions allocate and can throw, nam-runtime §2.2), change presets and oversampling
quality, and feed meters and the tuner, without ever blocking or allocating on that thread. The NAM reference plugin
gets this wrong: an unsynchronised `unique_ptr` move, and the old model freed on the audio thread (nam-runtime §2.5).
pluginval levels 7–9 test exactly these races, oversized blocks and audio-thread allocations (test-tooling §2.5).

## Decision

1. **Four thread roles:**
   - **Audio**: host or driver owned, the only real-time thread.
   - **Message**: JUCE message thread (UI, timers).
   - **Control**: any non-audio host thread calling state, prepare or parameter APIs. It may be the message thread.
   - **Worker**: one engine-owned thread that builds heavy objects and frees retired ones.

   v1 has no parallel DSP threads.
2. **The audio thread only reads atomics and pops or pushes lock-free SPSC queues.** All allocation, file I/O, parsing,
   `prepare()` of new blocks and deletion happen on the control or worker threads.
3. **Parameters:** the audio thread reads host-owned `std::atomic<float>` values (JUCE APVTS raw values, registered with
   the engine as plain `std::atomic<float>*`) once per block, then smooths them per sample inside `dsp-core`. No
   sample-accurate automation in v1 (it waits for JUCE AudioProcessor v2, framework-comparison §2.1).
4. **Heavy objects** (NAM model, IR/convolver, a whole `Chain` for a preset or quality change):
   1. built and `prepare()`d on the worker;
   2. published to the audio thread as an owning raw pointer inside a command, through the command SPSC queue;
   3. installed at a block boundary, with a crossfade;
   4. the old object goes to the **retire** SPSC queue (audio → worker) and is deleted on the worker.

   The audio thread never runs a destructor that frees memory.
5. **Crossfade:** model, IR and preset swaps fade over **25 ms** (round(0.025·fs) samples, 19 blocks at 48 kHz / 64),
   inside the 20–30 ms range from nam-runtime §3. 25 ms rather than 32 ms: it stays inside the researched range, and
   it keeps the two-instance CPU peak (REQ-703) as short as possible. **ADR-0004 is the single owner of this value.**
   Other documents (ADR-0002, ADR-0003, signal-flow.md, requirements.md) say "the ADR-0004 crossfade" and don't repeat
   the number. Swaps are allowed during playback, in the plugin and the standalone. This answers nam-runtime Q5.
6. **One producer and one consumer per queue.** Control-side calls are serialised by an `EngineController` (a non-RT
   mutex that the audio thread never touches). The controller only enqueues **jobs** to the worker (a non-RT job
   queue). The **worker is the sole producer** of the command queue, and the audio thread its sole consumer. The audio
   thread is the sole producer of the retire queue, and the worker its sole consumer. Every SPSC index update uses
   release on the producer side and acquire on the consumer side (threading.md §2). Naming: "worker" in this ADR is
   the thread other documents may call the "loader thread"; "control" is any non-audio host thread.
7. **The controller owns the "desired state".** `getStateInformation` and `setStateInformation` read and write the
   parameter atomics **synchronously** under the controller mutex. Only heavy objects are rebuilt asynchronously.
   Immediate round-trips (pluginval levels 6–8) are therefore deterministic (test-tooling §2.5, threading.md §6).
8. **The app splits oversized host blocks** into chunks of at most `maxBlock` before calling the engine
   (pluginval level 8, nam-runtime §3 "Block size").
9. **Generation tags:** every `prepare()` bumps a generation counter. Objects built for an old sample rate or block size
   are dropped on arrival (retired unused).

## Consequences

- \+ No locks, allocation or frees on the audio thread by construction. The RT checker (realtime-rules.md) verifies
  it in tests.
- \+ One worker thread keeps the design simple. Loads are serialised, so a preset with a NAM model and an IR builds in
  one job.
- − During a 25 ms swap, two models or chains run at once. CPU peaks at roughly steady state + one NAM instance
  (≤ +8 % of a core per nam-runtime §1.4). The 25 % budget applies to steady state; the swap peak gets its own
  benchmark gate (requirements.md, #21).
- − Applying a state is asynchronous (one worker job + one fade, typically tens of ms; *unverified* until measured).
  Hosts that render offline right after `setStateInformation` need the "drain" call described in threading.md §6.
- \+ The retire queue cannot overflow. Its capacity is ≥ cap(command queue) + max active objects, and the worker
  drains it before every command push (threading.md §4). No overflow array is needed.
