# ADR-0011: Amp models v1 (classic-DSP amps and NAM channels) and the Phase 5 plan

- **Status:** Proposed (2026-10-28)
- **Deciders:** `default/orchestrator` acting as architect (issue #166). Owner rule in force: never ask, decide and record.
- **Inputs:** [ADR-0002](0002-nam-runtime.md) (NAM runtime, `IAmpModel`, fixed `L_slot`, REQ-NAM-001…010),
  [ADR-0004](0004-threading.md) §4–§9 (worker builds, SPSC publish, 25 ms swap crossfade),
  [ADR-0005](0005-oversampling.md) (OS-2 region, D5 aliasing gates), [ADR-0007](0007-test-strategy.md) (bench, RT
  checker, golden tiers), [ADR-0009](0009-engine-graph-v1.md) D4/D5 (measured delays, `AmpChannel`, `AmpStub`),
  [`parameters.md`](../spec/parameters.md) §2.5, [`requirements.md`](../spec/requirements.md) REQ-406…412,
  [`tone-targets-a.md`](../research/tone-targets-a.md) findings 1, 3, 4, [`tone-targets-b.md`](../research/tone-targets-b.md).

## Context

Phase 5 (MASTER_PROMPT, phases) is "a NAM loader plus default amp models, the cab IR loader, and the full chain
within the CPU budget". The cab IR loader is done (#73 and earlier, ADR-0010). The graph (ADR-0009) runs every block
except the amp: each of the 3 amp channels is an `AmpStub` (identity plus `levelDb`) behind `engine::AmpChannel`
(`engine/include/osp/engine/AmpSlot.h`). ADR-0002 fixed the NAM side in detail, but was written before ADR-0009, so the
two interfaces differ:

| | ADR-0002 `IAmpModel` | ADR-0009 `AmpChannel` (in the code) |
|---|---|---|
| buffers | `process(in, out, n)`, no aliasing | `process(io, n)`, in place |
| parameters | none (a model is a fixed network) | `setParameters(const AmpParams&)` once per block |
| delay | `int getLatencySamples()` | `double getMeasuredDelaySamples()` (ADR-0009 D4 aligns on measured delays) |
| quality | load option (D9) | `prepare(fs, maxBlock, OversamplingQuality)` |

No classic amp exists yet, no NAM dependency is fetched, and no research note covers white-box amp topologies.
The tone research says four of the five targets are clean to mid-gain and one is high-gain (tone-targets-a
finding 1), so v1 needs a hi-fi clean, an edge-of-breakup/crunch and a high-gain lead.

## Decision

### D1. Layering

```
engine::AmpChannel (ADR-0009 slot, one per channel strip)
 └─ engine::ModelAmpChannel  (replaces AmpStub in Graph)
     ├─ active / fading-in  engine::IAmpModel*      (ADR-0002, owned via the ADR-0004 publish/retire queues)
     │    ├─ engine::ClassicAmpModel  → dsp-core osp::dsp::amp::* stages inside the OS-2 region (ADR-0005)
     │    └─ engine::NamAmpModel      → NAM core (PRIVATE to osp_engine) + engine/src/nam/RtResampler (ADR-0002 D4)
     ├─ NAM-channel controls: input trim from `gain`, static 3-band EQ, presence/depth shelves (parameters.md §2.5)
     ├─ levelDb ramp (as AmpStub)
     └─ pad to the slot delay D_slot (FractionalDelay)
```

- **dsp-core** gets a new folder `dsp-core/include/osp/dsp/amp/` (+ `dsp-core/src/amp/`) with the classic stages.
  No NAM, no engine types. Each stage is a self-contained block with its own unit tests.
- **engine** owns `IAmpModel`, `ClassicAmpModel`, `NamAmpModel`, `RtResampler` and `ModelAmpChannel`. NAM core,
  Eigen and nlohmann types stay out of `engine/include/` (ADR-0002 REQ-NAM-009).
- `AmpChannel` stays the graph's only view of the slot. The graph code changes only where it constructs the channel
  (`Graph.cpp` builds `ModelAmpChannel` instead of `AmpStub`; `AmpStub` stays for graph tests).

### D2. Reconciling the two interfaces (amends ADR-0002 D2)

- `IAmpModel::process(in, out, n)` keeps separate buffers. `ModelAmpChannel` owns two preallocated mono scratch buffers
  (one per model during a fade) and copies, so `AmpChannel::process(io, n)` stays in place.
- `IAmpModel` gains `double getMeasuredDelaySamples() const noexcept`: the 1 kHz group delay at host rate, measured in
  `prepare()` the way `DriveRegion` does (#104). `getLatencySamples()` stays and returns `ceil()` of it.
- **Slot delay.** `ModelAmpChannel::prepare()` computes one constant per (fs, quality):
  `D_slot = ceil(max(D_classic(fs, q), D_nam48(fs)))`, where `D_nam48` is the resampler's measured delay for a 48 kHz
  model (0 at 48 kHz). Every model is padded with a `FractionalDelay` of `D_slot − model delay`, so all models and all
  channels report the same integer `D_slot` and a swap crossfade is time-aligned (ADR-0002 D4, ADR-0009 D4). This is
  ADR-0002's `L_slot` expressed in measured delays.
- `setParameters(const AmpParams&)` is forwarded to `ClassicAmpModel` through a non-virtual setter on the concrete
  type (the channel knows which kind it holds). NAM models ignore everything except what D5 maps.
- Quality: `OversamplingQuality` selects the OS-2 factor for classic amps (ADR-0005 table: Live 4×/2×, HQ 8×). NAM
  quality stays a load option (ADR-0002 D9).

### D3. v1 classic model set

Three built-in models, matching the parameters.md §2.5 defaults:

| Id | Role | Covers |
|---|---|---|
| `osp.amp.clean_a` | hi-fi clean with lots of headroom, mild compression at high gain | the clean targets, compressed-clean leads |
| `osp.amp.crunch_a` | edge-of-breakup to master-volume crunch | low/mid-gain leads, crunch rhythm |
| `osp.amp.lead_a` | high-gain lead with a tight low end | the high-gain rhythm and lead targets |

- Display names are original and chosen in the preset/UI phase. No real amp, maker or artist name appears in ids,
  names, code comments, presets or UI (AGENTS.md rule 2). Research notes may say "aims at a tone similar to X".
- The **console** type (REQ-409, cab auto-bypass) and the **acoustic/piezo** voicing (tone-targets-a finding 4) are
  **deferred to a Phase 5 follow-up ticket** (P5-12). They reuse the clean model's stages.
- All three models are built from the same stage library: preamp gain stages → tone stack → power amp with negative
  feedback (presence/depth inside the loop, parameters.md §2.5) and supply sag. The topology of each stage, its
  constants and what each knob position means in dB are decided by the research ticket P5-1 and the stage tickets,
  not here. Constraints fixed now:
  - Static nonlinearities are precomputed in `prepare()` (tables or closed forms) and use 1st-order ADAA inside OS-2
    (ADR-0005 table). No per-sample allocation, table rebuild or transcendental beyond the stage's benchmark budget.
  - Knob changes follow [parameters.md §1.4](../spec/parameters.md#14-smoothing): target coefficient design and
    per-sample application are separate. A stage may design targets on changed values and glide coefficients per
    sample, or smooth parameters and design at a bounded adaptive sub-block cadence with per-sample coefficient
    ramps. Design may therefore run more than once per host block; applied coefficients must follow a stable
    per-processing-sample trajectory. The unchanged REQ-TEST-003 residual gate is ≤ −60 dBc against the specified
    per-sample double reference, not a licence to hold coefficients between updates. This clarification follows
    merged [PR #183](https://github.com/ArthurMateus/OpenSourcePedal/pull/183) and
    [PR #185](https://github.com/ArthurMateus/OpenSourcePedal/pull/185) (accessed 2026-10-28).
  - `bright` and `tight` are per-model switches with a 10 ms crossfade (parameters.md "xf 10").
  - Stage state is plain arrays; `reset()` is O(state) and allocation-free.

### D4. Pre-amp HPF (REQ-408)

REQ-408 lists a pre-amp HPF 20–300 Hz, but parameters.md §2.5 and the v1 table have no such key. Nothing is released,
so adding a host parameter is still free. **Decision:** add `amp.ch<N>.lowCutHz` (20…300 Hz, default 20 = off, log
skew, 20 ms smoothing, automatable) in ticket P5-9, applied at the channel input for both model kinds. `tight`
stays a model-specific voicing switch on top. P5-9 updates parameters.md, the v1 table and the host-ID test together.

### D5. NAM channels

Exactly as ADR-0002 D3–D9 and parameters.md §2.5 (input trim from `gain`, static post-EQ, `namLevelMode`, global
`amp.inputInterfaceLevelDbu`), with D2's measured-delay padding. Unchanged decisions: NAM core only, mono only,
`.nam` 0.5.0–0.7.0, models trained at rates whose delay exceeds `D_slot` are rejected, no third-party `.nam` ships,
NAM core `example_models` (MIT) are used in `tests/` only, with a row in `assets/LICENSES.md`.

### D6. Budgets and gates

| Item | Gate | Source |
|---|---|---|
| One classic amp, 48 kHz / 64, Live | ≤ 6% of one core (*unverified target*, measured in P5-8; raised here only by amendment) | this ADR |
| One NAM A1-Standard, 48 kHz / 64 | ≤ 8% of one core | ADR-0002 D7, REQ-NAM-008 |
| Full chain with a real amp, steady state | ≤ 25% of one core | AGENTS.md §4 |
| Full chain during an amp swap fade | steady chain + one amp model | ADR-0002 D7 |
| Aliasing, each classic stage and each model | ADR-0005 D5 table (Live ASR ≤ −60 dB for f0 ≤ 2.5 kHz, ≤ −40 dB up to 8 kHz; HQ warn) | ADR-0005 |
| Goldens | classic amps: Tier 1 null; NAM: Tier 2 spectral | ADR-0007 §3, ADR-0002 D3 |
| Latency | reported total within ±1 sample of the measured impulse, every rate, every quality, every model kind | REQ-301, ADR-0009 D4 |
| RT safety | RT checker clean under model swap and parameter fuzz; no NaN/Inf/denormal | AGENTS.md §4, REQ-NAM-002 |

Benchmarks are added as new self-registering files (`tests/bench/entries/amp_*.cpp`, `tests/bench/budget.d/amp.json`,
`nam.json`), never by editing a shared list (#115).

### D7. Ticket slicing, order and file ownership

Each slice is one issue and one PR of about 400 changed lines or fewer. "Owns" lists the files a slice may create or
edit; two slices that run at the same time never own the same file.

| # | Slice | Role | Blocked by | Owns |
|---|---|---|---|---|
| P5-1 (#167) | Research: white-box amp stages (triode/pentode models, tone stacks, NFB + presence/depth, sag, ADAA cost) with sources, licence-clean references only | researcher | — | `docs/research/classic-amp-models.md` |
| P5-2 (#168) | NAM core + Eigen dependency pins, `RtResampler` fork, include-isolation check (REQ-NAM-009) | dsp-engineer | — | `cmake/OspDependencies.cmake`, `THIRD_PARTY.md`, `engine/src/nam/RtResampler.*`, `tests/engine/test_rt_resampler.cpp`, include-isolation check as a CTest test under `tests/tools/` (not a workflow edit) |
| P5-3 (#169) | `IAmpModel` (+ D2 amendment) and `NamAmpModel`: load, prepare, process, snapshot `reset()`; REQ-NAM-001/003/005/010; NAM bench entry | dsp-engineer | P5-2, #83 | `engine/include/osp/engine/AmpModel.h`, `engine/src/nam/NamAmpModel.*`, `tests/engine/test_nam_*.cpp`, `tests/fixtures/nam/`, `assets/LICENSES.md` row, `tests/bench/entries/amp_nam.cpp`, `tests/bench/budget.d/nam.json` |
| P5-4 (#170) | NAM at non-native rates: resampling + measured delay, REQ-NAM-002/004 | dsp-engineer | P5-3 (#169) | `engine/src/nam/*` (after P5-3), `tests/engine/test_nam_rates.cpp` |
| P5-5 (#171) | dsp-core preamp gain stage (static curve + ADAA1 + coupling HPF, bias shift) | dsp-engineer | P5-1 (#167) | `dsp-core/*/amp/PreampStage.*`, `tests/dsp/test_amp_preamp.cpp` |
| P5-6 (#172) | dsp-core tone stack (passive 3-knob stack per P5-1, bilinear, per-block coefficients) | dsp-engineer | P5-1 (#167) | `dsp-core/*/amp/ToneStack.*`, `tests/dsp/test_amp_tonestack.cpp` |
| P5-7 (#173) | dsp-core power amp (push-pull stage, NFB loop with presence/depth, sag) | dsp-engineer | P5-1 (#167) | `dsp-core/*/amp/PowerAmp.*`, `tests/dsp/test_amp_poweramp.cpp` |
| P5-8 (#174) | `ClassicAmpModel` in OS-2 with the three D3 voicings, `bright`/`tight`; aliasing, THD-vs-gain, bench | dsp-engineer | P5-5, P5-6, P5-7, #83 | `engine/src/amp/ClassicAmpModel.*`, `dsp-core/*/amp/Voicings.*`, `tests/engine/test_classic_amp.cpp`, `tests/bench/entries/amp_classic.cpp`, `tests/bench/budget.d/amp.json` |
| P5-9 (#175) | `ModelAmpChannel` in the graph: D2 padding, NAM-channel controls, `levelDb`, `lowCutHz` (D4); latency tests at every rate/quality; goldens updated with reasons | dsp-engineer | P5-3, P5-8 | `engine/src/graph/AmpSlot.cpp`, `engine/include/osp/engine/AmpSlot.h`, `engine/src/graph/Graph.cpp`, `engine/src/Params.cpp`, `docs/spec/parameters.md` §2.5, `tests/engine/test_amp_channel.cpp`, `tests/golden/` (created here if still absent, ADR-0007 §3) |
| P5-10 (#176) | Model loading and hot-swap through ADR-0004 queues (25 ms aligned fade), `amp.ch<N>.model` resource refs in state, REQ-NAM-006 | audio-io-engineer | P5-9 (#175) | `engine/src/Engine.cpp`, `engine/src/State.cpp`, `app/src/*` loader glue, `tests/engine/test_amp_swap.cpp` |
| P5-11 (#177) | NAM calibration: global input interface level, `namLevelMode`, REQ-NAM-007 | audio-io-engineer | P5-9 (#175) | `app/` settings, `engine/src/nam/Calibration.*`, `tests/engine/test_nam_calibration.cpp` |
| P5-12 (#178) | Console type (REQ-409) and acoustic voicing | dsp-engineer | P5-8 (#174) | `dsp-core/*/amp/Console.*`, voicing entries, tests |
| P5-13 (#179) | Phase 5 gate: full chain with each classic model and NAM A1-Standard under budget, goldens, demo clips in `docs/demos/phase5/` | qa-engineer | P5-9, P5-10, P5-11 | `tests/bench/entries/chain_amp.cpp`, `docs/demos/phase5/`, QA notes |

- **Parallel lanes.** P5-1 ∥ P5-2 start now. After P5-1: P5-5 ∥ P5-6 ∥ P5-7. P5-3 → P5-4 runs alongside the stages.
  P5-8 and P5-3 join at P5-9; then P5-10 ∥ P5-11 (disjoint files), then P5-13. P5-12 can run any time after P5-8.
- **Benchmark API.** #83 (in progress on another account) may change `tests/bench/bench_common.h` and the
  `OSP_BENCH`/`measure` helpers, and it rewrites `r0` in every `budget.d/*.json`. So every slice that adds a bench entry
  (P5-3, P5-8, P5-13) is **blocked by #83**, and none of these slices edits `bench_common.h`, `osp_bench.cpp`,
  `tools/bench/*` or any file under `.github/workflows/`.
- **Shared files.** Only P5-9 edits `Graph.cpp`, `AmpSlot.*`, `Params.cpp` and parameters.md; only P5-10 edits
  `Engine.cpp`/`State.cpp`; only P5-2 edits `cmake/OspDependencies.cmake` and `THIRD_PARTY.md`; `assets/LICENSES.md` is
  edited by P5-3 only. Later edits to these files wait for the owning slice to merge.

### Amendment A1 (2026-10-28): D7 interface ownership and engine source discovery

The D7 table above records the original plan. This amendment supersedes the following assignments without
rewriting that history (coordination sources accessed 2026-10-28):

For coefficient timing, D3 and parameters.md §1.4 above supersede the historical "per-block coefficients" label
in P5-6; the original row is retained as part of the plan's history.

- **P5-8 (#174) creates `engine/include/osp/engine/AmpModel.h`:** `IAmpModel` per D2 (including
  `getMeasuredDelaySamples()`), `AmpModelInfo` and the `makeClassicAmp` factory declaration, with no NAM types.
  **P5-3 (#169) only extends that header** later with its NAM declarations (`loadNamModel`/`NamAmpModel`); it must
  not replace the interface. Sources: [#174 ownership decision](https://github.com/ArthurMateus/OpenSourcePedal/issues/174#issuecomment-6479505891),
  [#169 coordination](https://github.com/ArthurMateus/OpenSourcePedal/issues/169#issuecomment-6479506656).
- **Engine source discovery is recursive:** `file(GLOB_RECURSE ... CONFIGURE_DEPENDS src/*.cpp)`, following #115,
  so new engine sources need no shared source-list edit. #174 part 1 owns only the source-list change in
  `engine/CMakeLists.txt`; #168 retains its separate PRIVATE `osp_nam_core` link and does not edit that list.
  This records the [#174 source-discovery decision](https://github.com/ArthurMateus/OpenSourcePedal/issues/174#issuecomment-6479574275),
  not a claim that the still-active implementation has already merged.

## Consequences

- + One interface for the graph (`AmpChannel`) and one for model kinds (`IAmpModel`); NAM and classic models swap with
  an aligned fade and a constant reported latency.
- + The stage library is testable block by block before any model exists, and three stage tickets can run in parallel.
- + Bench and CI files stay with #83/#165, so Phase 5 PRs do not conflict with that work.
- − Every model carries the slot padding, so at 44.1 kHz a classic amp carries the NAM resampler's delay (inherited
  from ADR-0002).
- − Two scratch copies per channel during a fade (small: 2 × maxBlock floats per channel).
- − The 6% classic-amp budget is a target, not a measurement, until P5-8.

## Open items

| Item | Owner | When |
|---|---|---|
| Stage topologies, constants and knob laws per model | researcher (P5-1), dsp-engineer (P5-5…P5-8) | P5-1 (#167) |
| Native-rate adaptation exactness, snapshot mechanism for `reset()` | dsp-engineer | P5-3/P5-4 (ADR-0002 open items) |
| Whether the classic amp HQ (8×) fits the full-chain budget at 96 kHz | dsp-engineer | P5-8 measurement |
| Display names of the three models | preset-designer | Phase 6 |
| Own `.nam` captures to ship | preset-designer + dsp-engineer | after Phase 5 (ADR-0002 D8) |
