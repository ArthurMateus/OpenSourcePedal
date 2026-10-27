# ADR-0009: Engine v1 signal graph

- **Status:** Proposed (2026-10-27)
- **Deciders:** `default/architect` (issue #95). Owner decisions in force: no UI yet (Phase 6), merge green PRs.
- **Inputs:** [`signal-flow.md`](../spec/signal-flow.md) (topology §1, latency §4, transitions §5),
  [`parameters.md`](../spec/parameters.md), [`threading.md`](../spec/threading.md),
  [`realtime-rules.md`](../spec/realtime-rules.md), [ADR-0002](0002-nam-runtime.md) D4 (`L_slot`),
  [ADR-0004](0004-threading.md) §5 (swap crossfade), [ADR-0005](0005-oversampling.md) D4 (latency),
  [ADR-0007](0007-test-strategy.md) (REQ-TEST-003/004), issue #95 and Moth's comment on it (acct3/code-reviewer,
  from the #104 review).

## Context

`engine::Chain` is a pass-through (output gain only). The Phase 4 blocks now exist in `dsp-core/`: input stage,
gate, compressor, the OS-1 drive region, cab, EQ, modulation, delay, reverb, doubler, output stage and tuner tap.
Phase 5 needs them wired into one graph, with latency alignment, so the plugin is playable and the full-chain CPU
budget (AGENTS.md §4) can be measured. The amp models are Phase 5 work and do not exist yet.

The blocks are not uniform. Some have their own enable crossfade (`Gate`, `DriveStage`, `Cab`, `Modulation`,
`Delay`, `Reverb`, `Doubler`), others only a hard `setBypassed` (`InputStage`, `Compressor`, `Eq`). Only OS-1 has
a fractional delay (`DriveRegion::getMeasuredDelaySamples()`, +0.47 at 44.1/48 kHz Live, #104).

Moth's comment on #95: rounding per block can leave the reported total more than ±1 sample from the measured
delay (REQ-301). Report `round(Σ D_measured)`, align parallel paths from the same fractional sums, and add an
end-to-end REQ-301 impulse test with every oversampled block enabled.

## Decision

### D1. One `Graph` object, fixed v1 topology, new files under `engine/`

`osp::engine::Graph` (`engine/include/osp/engine/Graph.h`, `engine/src/graph/*.cpp`) owns every block **by value**
(threading.md §2). Topology, fixed for v1 (signal-flow §1):

```
in (mono) → input → [tuner tap] → gate(pre) → comp → OS-1 {drive1 → drive2} → mod(pre, mono)
   ├─ clean DI tap ──────────────────────────────── delay K_clean ─────────────────────┐
   └─ strip[active] { amp slot → gate(post) → cab (mono → stereo) → EQ } ── strip xf ──┴─ blend → mod(post)
      → delay → reverb → doubler → output (level, limiter, meters, NaN guard) → out (stereo)
```

- **Mono to stereo** at the cab, as in signal-flow §2. `Graph::process(const float* in, float* outL, float* outR, n)`.
  `Engine` maps the host input bus to mono (REQ-207: the selected channel; the default is channel 1) and owns the
  graph. In-place is allowed (`in == outL`).
- **Amp channel strips.** `amp`, `cab` and `eq` hold per-channel state (`amp.ch<N>`, `cab.ch<N>`, `eq.ch<N>`), so
  the graph holds **3 strips** of {amp slot, cab, EQ}. Only the active strip runs. An `amp.channel` change runs both
  strips and crossfades their stereo outputs over the ADR-0004 swap time (linear: same input, correlated). The strip
  that fades in is `reset()` first (cold start, as `Eq` does for a voicing). `amp.cabLink = false` and
  `eq.followChannel = false` fan ch1's values out to all three strips (parameters.md §3.1, §3.2). The control side
  builds one convolver per strip, because a convolver holds state and cannot run in two strips.
- **Modulation position** (`mod.position`, preset-only). One `Modulation` instance. A change fades the block's wet
  out with its own `setEnabled(false)` (10 ms), moves it once that has settled, `reset()`s it and fades it back in.
  The dry signal never dips. This replaces "crossfades like a channel switch" in signal-flow §1: running a second
  instance would double the state for a preset-only switch.
- **`Chain` stays** as the minimal pass-through `osp-render` drives, until the preset loader gives `osp-render`
  a `--preset` path. It is then removed. `Engine` moves to `Graph` in PR 6 (D10).

### D2. The block contract the graph needs

Every node gives the graph: `prepare(sampleRate, maxBlock[, quality])` (non-RT, allocates), `reset()`,
RT-safe setters called once per block, a `noexcept` process, `setBypassed(bool)` (hard bypass: an exact,
latency-aligned copy of the input, REQ-TEST-004), and its delay. The graph reads **`double
getMeasuredDelaySamples()`** where a block has one (OS-1, and the amp slot), and `getLatencySamples()` (0)
otherwise. The graph does not change `dsp-core/` for #95. It adapts the existing APIs in one place per block
(`engine/src/graph/`), so a later block API change touches one adapter.

### D3. Per-block bypass

| Block | `<id>.enabled` maps to | Settled disabled |
|---|---|---|
| gate | graph-owned 10 ms ramp `e` in the gain law (D6) | factor exactly 1 |
| drive1, drive2 | `DriveStage::setEnabled` (10 ms, inside OS-1) | identity stage at the high rate; OS-1 keeps running, so the latency and phase never change (REQ-303) |
| cab, mod, doubler | the block's own `setEnabled` (10 ms) | exact dry |
| delay | `setSoftBypass(!enabled)`, with `setEnabled(true)`: trails (signal-flow §5) | exact dry once the tail has died |
| reverb | `setEnabled` (its soft bypass: trails) | exact dry once the tail has died |
| input, comp, eq | graph-owned `dsp::Crossfader` (10 ms, linear) between a copy of the block's input and its output | exact dry; the block is not processed and is `reset()` when it is enabled again |

`amp` and `output` have no `enabled` (parameters.md §1.6). The **hard bypass** used by tests,
`Graph::setBypassedAll(bool)`, calls every block's `setBypassed` and makes the amp-slot stub an identity. The
graph output is then the input delayed by `L_total` exactly. That is the "all-bypassed graph" of #95, and it nulls
(REQ-TEST-004). A user-disabled chain still runs OS-1 (IIR in Live), so it is tested the ADR-0005 D4 way: magnitude
and 1 kHz group delay, not a null.

### D4. Latency: rounded once, from cumulative fractional delays (Moth's ask (a))

The graph has these tap points. `P(x)` is the measured delay (base-rate samples, double) from the input to `x`,
added up in `prepare()`:

| Point | `P(x)` |
|---|---|
| gate (key in, pre-gain) | 0 |
| amp input (after OS-1 and pre mod; clean DI tap) | `D_os1` |
| amp output (post-gate tap) | `D_os1 + D_slot` |
| EQ output (clean blend) and output | `D_os1 + D_slot + D_cab` |

Each point gets **one integer**, `R(x) = round(P(x))` (`std::lround`, halves away from zero). Then:

- **Reported latency:** `L_total = R(output) = round(D_os1 + D_slot + D_cab)`. `Graph::getLatencySamples()`
  returns it, `Engine::getLatencySamples()` forwards it, and the app passes it to `setLatencySamples` in
  `prepareToPlay` (it already does, `app/src/PluginProcessor.cpp`). It changes only in `prepare()` (sample rate,
  max block, quality).
- **Parallel path from a to b:** an integer delay of `R(b) − R(a)`.
  - post-gate gain: `K_gate = R(amp output) − R(gate) = round(D_os1 + D_slot)`
  - clean DI: `K_clean = R(EQ output) − R(amp input) = round(D_os1 + D_slot + D_cab) − round(D_os1)`

The sums telescope, so every input-to-output path has an integer delay that adds up to exactly `L_total`. Each tap
is within ±0.5 sample of the true fractional delay at that point. The reported value is within ±0.5 of the
measured 1 kHz delay (REQ-301 asks ±1).

This **amends** signal-flow §4 (`L_total` was the sum of per-block `round(D)`) and the ADR-0005 D4 bullet "the
engine sums the slot latencies". The integer `getLatencySamples()` of a block stays as it is: it is the block's own
contract for its own tests and its hard-bypass padding. #95's "exactly `L_os1 + L_slot`" becomes "exactly
`K_gate`". It is the same number whenever the slot delay is an integer, as in the v1 stub.

### D5. The amp slot until Phase 5

The amp slot is an engine interface, `engine::AmpChannel` (virtual; a virtual call to an already-built object is
RT-allowed, realtime-rules §1): `prepare`, `reset`, `setParameters(const AmpParams&)`, `process(float* io, n)`,
`setBypassed`, `getMeasuredDelaySamples()`. v1 ships **`AmpStub`**: identity, plus the channel's `levelDb` (20 ms
linear ramp), so the slot is parameter-addressable. Every other `amp.ch<N>.*` value is stored and ignored. Its
delay is 0. Tests also build it with a **fractional test delay** (`dsp::FractionalDelay`, for example 2.37
samples), so the alignment code runs with a non-integer `D_slot` long before Phase 5. The graph asserts in debug
builds that all three strips report the same delay (ADR-0002 D4: one `L_slot` for every channel and model kind).
Phase 5 replaces the stub with the classic-DSP/NAM slot behind the same interface. Its model swap is the ADR-0004
install, not a graph change.

### D6. `gate.postAmp`: delayed gain, moved in the log domain (xf 10)

The gate always runs at its slot and computes its gain `g(t)` from its own input with `Gate::computeGain` (the
key). Two 10 ms linear ramps: `e` (`gate.enabled`) and `x` (`gate.postAmp`, 0 = pre, 1 = post). With
`h(t) = 1 − e(t)·(1 − g(t))`:

```
pre-amp factor    a(t) = h(t)^(1 − x(t))                  applied at the gate slot
post-amp factor   p(t) = h(t)^x(t),  b(t) = p(t − K_gate)  delayed, applied after the amp slot, before the cab
```

- Settled (`x` is 0 or 1) the factors are exactly `h` and 1, with no `pow` call. During the 10 ms fade the
  attenuation **moves** from before the amp to after it: through a linear amp, `a(t − K)·b(t) = h(t − K)` for any
  `x` trajectory. So the toggle is level-neutral by construction, and the xf-10 test can check it at REQ-TEST-003's
  −60 dBc residual against the static gate. Through a nonlinear amp the two placements sound different, and the
  move is a 10 ms glide with no step.
- Delaying the gain instead of the key gives the same result, because the gate is time-invariant. It also keeps
  one detector state for both taps, so a toggle never restarts the envelope.
- This is the "xf 10" of parameters.md §2.2. A crossfade of two full paths would need two amp slots running at
  once, and a NAM model holds state.
- `g` is ≥ −90 dB (the range minimum). The graph clamps `h ≥ 1e-6` before `pow`.

### D7. Clean DI blend

`clean(t)` = the amp input, delayed by `K_clean` (D4), duplicated to both channels. After the strip crossfade:
`out = (1 − β)·wet + β·clean`, with `β = amp.cleanBlendPct / 100` on a 20 ms linear ramp. `β = 1` is exactly the
delayed amp input (REQ-424 null). The blend never changes `L_total`.

### D8. Parameter fan-out

- **Table:** `engine/include/osp/engine/Params.h` lists every v1 id from parameters.md, with range, default, skew,
  kind (continuous, bool, enum with its value list) and visibility (`H`/`P`). Per-channel ids are expanded
  (`amp.ch1.gain` … `eq.ch3.levelDb`). Reserved ids (`transpose.*`) are listed and flagged, not in the host list.
  Resources (`amp.ch<N>.model`, `cab.ch<N>.ir1/ir2`) are not float parameters. They are objects, installed through
  the ADR-0004 path (`Graph::setCabIr`, `popRetired`). The table test checks 226 host-visible ids (parameters.md §5),
  unique ids, defaults in range, and the REQ-PAR-003 normalized ↔ plain round trip. The REQ-PAR-001 comparison
  against a generated `tests/spec/parameters.json` stays with the app parameter-layout work (PR 7).
- **Values:** `engine::ParamStore` holds one `std::atomic<float>` per table entry (plain value; enum = index;
  bool = 0/1), initialised to the defaults. A slot can be **bound** to an external atomic (the APVTS raw value, in
  PR 7). The audio thread reads every entry once per block, relaxed (threading.md §3), and the graph calls the
  block setters. Preset-only values are written by the control side into the store (the preset loader, later).
  Enums are rounded and clamped, and bools are `> 0.5`.
- **Global settings** (not parameters) reach the graph as setters: `quality` in `prepare()`, and RT-safe
  `setTempoBpm`, `setMonoSum` and `setTunerActive` (tuner mute, REQ-419).

### D9. Real-time rules

`Graph::process` and everything under it is `noexcept` and `OSP_RT_NONBLOCKING`. `prepare()` is the only call that
allocates. It sizes every scratch buffer for `maxBlock` (mono: main, key/gain factors, clean, dry copies; stereo:
two strip outputs, clean). It sizes `K_gate` and `K_clean` delay lines from the values it has just computed, and it
pre-touches every buffer (RT-12). `process(n)` handles any `0 ≤ n ≤ maxBlock`. `Engine` already splits larger host
blocks. The RT checker (C1–C4, C8) runs on the whole graph: rates {44.1, 48, 88.2, 96} kHz, block sizes
{0, 1, 32, 64, 127, 512, 2048}. A parameter fuzz on every table entry (NaN, ±Inf, out of range) asserts finite
output and no `nanDetected`. Denormals are flushed by the caller (RT-8).

### D10. Stacked PR plan

Each PR is about 400 changed lines or fewer, branches from the one below it, and is merged bottom-first by the
merge train. A PR that needs `origin/main` changes (#61, #133–#137, #115) merges `origin/main` in. It never rebases.

| # | Branch | Content | Acceptance criteria of #95 |
|---|---|---|---|
| 0 | `architect/95-graph-design` | This ADR; signal-flow §1/§4/§5, ADR-0005 D4 and parameters.md amendments | — |
| 1 | `engine/95-params` | `Params.h` table + `ParamStore` (bind, defaults, normalized ↔ plain), tests | table half of "every block parameter-addressable"; REQ-PAR-003 |
| 2 | `engine/95-graph-core` | `Graph`, `AmpChannel`/`AmpStub`, topology, D8 fan-out and D3 bypass for every block on `main`, strips and channel crossfade, mod position move | "every block reachable and parameter-addressable, unit test per block id" |
| 3 | `engine/95-latency` | D4 alignment, clean DI (D7), hard-bypass pads, fractional stub delay in tests | "all-bypassed graph nulls against the input delayed by `L_total`"; "reported latency equals the measured impulse delay at every rate"; Moth (b): REQ-301 impulse test, Live, 44.1/48 kHz, both drives on; REQ-424 null |
| 4 | `engine/95-gate-tap` | D6 gate gain law, `K_gate` delay line | "`gate.postAmp` edge at exactly `K_gate` (±0) at 4 rates"; "xf-10 tap switch under signal ≤ −60 dBc" |
| 5 | `engine/95-post-blocks` | After #134 and #137 merge: the `delay` block (D3 trails) and the reverb spring/shimmer modes in the fan-out | `delay.*` reachable and addressable |
| 6 | `engine/95-engine` | `Engine` owns the `Graph` (mono in, stereo out); full-chain osp-bench entry + `budget.json`; RT matrix and parameter fuzz on the full graph | osp-bench entry; "RT checker clean; NaN/Inf fuzz on the full graph"; Moth (a) end to end: `Engine::getLatencySamples() = round(Σ D)` |
| 7 | `engine/95-app-params` | After #61 merges: the APVTS layout from the table, binding host atomics into the `ParamStore`, REQ-PAR-001 spec list, pluginval | the app reports `L_total` (`setLatencySamples`) and exposes the 226 parameters |

Not in #95 (follow-up tickets): the `EngineController`, the command/retire queues and the worker
(threading.md §4); preset load and the whole-chain preset crossfade (two graphs, ADR-0004); extending the plugin
state blob (#61) to every parameter; the real amp slot (Phase 5).

## Consequences

- \+ The reported latency is within ±0.5 sample of the measured delay for any mix of fractional block delays, and
  every parallel path lands on the same integer grid.
- \+ The graph holds the blocks by value, so a whole graph is one object for the ADR-0004 preset swap later.
- \+ The tap move and the clean blend are exact at their end points (factors of exactly 1 or 0), so bypass and
  blend nulls hold.
- − Three strips triple the cab/EQ memory and the control side builds three convolvers per IR when the cab is not
  linked. Only one strip runs outside a 25 ms switch.
- − OS-1 runs even with both drives disabled (CPU), so that bypassing never moves the 1 kHz delay.
- − Through a nonlinear amp, a `gate.postAmp` toggle is a 10 ms glide between two different sounds, not a
  crossfade of two complete renders.

## Alternatives considered

- **Sum of per-block `round(D)`** (signal-flow §4 before this ADR): rejected by Moth's comment. Two IIR regions with
  +0.47 each would be 0.94 sample off, and a third breaks REQ-301's ±1.
- **Pad every fractional block to an integer with a fractional delay:** an allpass or Lagrange pad colours the top
  octave and costs CPU per block. Rounding once costs nothing.
- **Two full paths for the gate tap (xf of complete renders):** needs a second amp slot running, which is
  impossible for one stateful NAM instance.
- **One cab and one EQ re-parameterised on a channel switch:** a step in the IR and the EQ curve. ADR-0004 asks for
  a crossfade between the old and new states.
