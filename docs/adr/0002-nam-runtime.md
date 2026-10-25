# ADR-0002: NAM runtime and the `IAmpModel` interface

- Status: **Proposed** (2026-10-25). It becomes Accepted when the PR merges.
- Deciders: architect (acct1/Sable). Issue #26.
- Rests on: [`docs/research/nam-runtime.md`](../research/nam-runtime.md) (#6, cited as NR §x),
  [`docs/research/redistributable-assets.md`](../research/redistributable-assets.md) (RA §x),
  [`docs/research/oversampling.md`](../research/oversampling.md) (#10, OS §x), ADR-0001, ADR-0004 (#37, threading and
  handoff), ADR-0005 (#27, oversampling), ADR-0006 (pins and the dependency list), ADR-0007 (benchmarks, RT checker,
  golden tiers), `docs/spec/realtime-rules.md` (RT-x, #37), and `docs/spec/requirements.md` (REQ-xxx, #40).

## Context

The product is hybrid: amps use `.nam` neural models, and everything else is classic DSP (`docs/MASTER_PROMPT.md`).
The research found:
- NAM core v0.6.0 (MIT) is the reference runtime. It covers every `.nam` version from 0.5.0 to 0.7.0 and every
  architecture (WaveNet A1/A2, LSTM, ConvNet, Linear, SlimmableContainer) (NR §2.2–2.3).
- NeuralAudio (MIT) is faster but adds a second code path. RTNeural alone cannot run `.nam` WaveNet (NR §1.2–1.3).
- An A1-Standard model costs about 2.8% of a core at 48 kHz/64 in an optimised port. The C++ cost is *unverified*
  (NR §2.4).
- `get_dsp()`, `Reset()`, `prewarm()` and `SlimmableModel::SetSlimmableSize()` ("Not real-time safe" in
  `NAM/slimmable.h` @ v0.6.0) allocate. The reference plugin's model swap is not real-time safe (NR §2.2, §2.5).
- Models are mono, with zero latency at their native rate. Other rates need resampling (NR §2.2, §2.5).
- AudioDSPTools' `ResamplingContainer::ProcessBlock` takes a `std::function`, can `throw`, and writes to `std::cerr`,
  so it is not RT-safe as shipped (review of #39, from reading the source at the ADR-0006 pin).
- No redistributable high-quality capture exists. The NAM core `example_models` (MIT) may be used in tests only
  (RA §1.3, N1).

## Decision

### D1. Runtime
- **v1 uses NAM core only**, pinned by ADR-0006 and linked `PRIVATE` into `osp_engine`. No NAM, Eigen or nlohmann
  type appears in a public `engine/` header.
- **NeuralAudio and RTNeural are not fetched in v1** (this answers NR open question 1). Re-open condition:
  REQ-NAM-008 fails in Phase 3. The owner is the dsp-engineer, who adds a second `IAmpModel` backend through an
  ADR-0002 amendment. No other code changes, because of D2.

### D2. `IAmpModel` (in `engine/`, no JUCE, no NAM types)

```cpp
namespace osp::engine {
struct AmpModelInfo {                 // built on the worker at load time; immutable afterwards.
    std::string  architecture;        // "WaveNet", "LSTM", ... The audio thread never copies this struct.
    double       trainingSampleRate;  // 48000 if the file has none (D4)
    std::optional<double> inputLevelDbu, outputLevelDbu, loudnessDb;
    bool         supportsQuality;     // slimmable (A2) models
};
struct AmpLoadOptions { float quality01 = 1.0f; };  // D9

class IAmpModel {
public:
    virtual ~IAmpModel() = default;   // worker thread only (ADR-0004 §4)

    // Worker/control thread. May allocate. Must be called before process(). Sizes every buffer for maxBlockSize,
    // runs the model's prewarm, and stores a snapshot of the prewarmed state (used by reset()).
    virtual void prepare(double hostSampleRate, int maxBlockSize) = 0;

    // Audio thread. RT-safe (RT-x rules). Mono. `in` and `out` must not alias (the engine provides separate buffers).
    // numSamples == 0, or a call before prepare(): no-op, and `out` is zero-filled for numSamples (if > 0).
    // 0 <= numSamples <= maxBlockSize; larger blocks are split by the app (ADR-0004 §8).
    virtual void process(const float* in, float* out, int numSamples) noexcept = 0;

    // Audio thread. RT-safe, O(state size): restores the snapshot taken in prepare() with memcpy. It never runs the
    // model, so repeat renders are bit-identical (ADR-0007 REQ-TEST-004).
    virtual void reset() noexcept = 0;

    // Any thread. The model's own latency in host-rate samples, an integer. Fixed after prepare().
    virtual int  getLatencySamples() const noexcept = 0;

    virtual const AmpModelInfo& info() const noexcept = 0;
};
// Worker thread. They throw osp::engine::ModelLoadError with a user-readable message.
std::unique_ptr<IAmpModel> loadNamModel(const std::filesystem::path&, const AmpLoadOptions& = {});
std::unique_ptr<IAmpModel> makeClassicAmp(ClassicAmpId);  // dsp-core based, see D8
}
```

- There is no `setQuality()` on the audio thread (D9).
- If NAM core's state cannot be snapshotted through its public API, `NamAmpModel` keeps a second, prewarmed instance
  per model as the snapshot source. The first NAM ticket decides which approach to use and measures it
  (REQ-NAM-010).

### D3. Accepted files and architectures
- `.nam` file versions 0.5.0 to 0.7.0, as accepted by NAM core's version checker. Anything else is rejected with
  "unsupported model version X" (NR §2.3).
- Any architecture NAM core loads is accepted. Benchmarks and goldens cover A1-Standard, A1-Nano, A2-Full, A2-Lite and
  LSTM 1×16. NAM goldens use ADR-0007's **Tier-2** spectral gate (neural blocks, SIMD variance), not the Tier-1 null.
- **Mono only:** `NumInputChannels() == 1 && NumOutputChannels() == 1`. Anything else is rejected (NR §2.5).

### D4. Sample rate, resampler and fixed slot latency
**Resampler.** For host rate ≠ model rate, `NamAmpModel` wraps the model in `engine/src/nam/RtResampler`. This is a
local fork of AudioDSPTools' `ResamplingContainer` (MIT; the original header and licence are kept, and the source is
recorded in `THIRD_PARTY.md`) with these changes:
- The `std::function` is replaced by a template callable (the NAM `process` call, capture-free).
- Both `throw` branches in `ProcessBlock` and the `std::cerr` underflow message are replaced by an
  `std::atomic<uint32_t>` error counter plus zero-fill. The control thread polls the counter and logs it.
- Every buffer is sized in `prepare()` from `maxBlockSize` and the rate ratio, so those branches are unreachable by
  construction. REQ-NAM-002 asserts that the error counter stays at 0.
- Lanczos A = 12, as upstream (NR §2.5).

**Native adaptation** (`SupportsArbitrarySampleRate()`, for example at 96 kHz with a 48 kHz model) is used only if
REQ-NAM-004 shows it is exact. Until then, every host rate ≠ model rate is resampled. NR open question 2 is deferred
to the first NAM ticket.

**Fixed amp-slot latency** (B3 of the review; satisfies REQ-303):
- The engine's amp slot reports a constant `L_slot(fs, quality)` = max(`L_res(fs, 48 kHz)`, `L_classic(fs, quality)`),
  computed in `prepare()`. `L_res` is the fork's latency for a 48 kHz model at host rate fs (0 at fs = 48 kHz).
  `L_classic` is the classic amps' oversampler latency (ADR-0005 D4).
- Every amp model (NAM native, NAM resampled, or classic) is **padded with a preallocated delay** up to `L_slot`. So:
  - a swap never changes the reported latency (no `setLatencySamples` from the audio thread, RT rules);
  - the two signals in a crossfade are time-aligned (no comb filtering).
- A model whose own latency at fs exceeds `L_slot` (for example a 44.1 kHz-trained model on a 48 kHz host) is
  **rejected in v1** with the message "model sample rate X is not supported at host rate Y". Revisit if user reports
  show such models are common (owner: dsp-engineer).
- A model without `sample_rate` is treated as 48 kHz (NR §2.3).
- NAM runs at its native rate, without oversampling (ADR-0005 D7).

### D5. Level calibration
- `amp.inputInterfaceLevelDbu`: −60…+60 dBu, default **12 dBu**. It is an app setting, not in presets, and not
  automatable. Input trim = `inputInterfaceLevelDbu − info.inputLevelDbu` when present, otherwise 0 dB (NR §2.5).
- `amp.outputMode` ∈ {Raw, Normalized, Calibrated}, per preset.
  - Normalized applies `−18 dB − loudnessDb` (NR §2.5).
  - Calibrated applies `outputLevelDbu − inputInterfaceLevelDbu`, which **assumes the interface's output calibration
    equals its input calibration**. A separate `outputInterfaceLevelDbu` is deferred (owner: audio-io-engineer).
  - The default is Normalized when `loudnessDb` is present, Raw otherwise. If metadata is missing, Raw is used and
    the UI greys out the option.
  - Normalized only levels model against model. Preset-level −18 LUFS-I normalisation (ADR-0007 REQ-TEST-005) still
    applies on top.
- Gain changes are smoothed. `parameters.md` owns the time.

### D6. Loading and hot-swap
The rules are **ADR-0004 §4–§9**: the worker builds and `prepare()`s, the command SPSC queue publishes, the model is
installed at a block boundary with a **25 ms crossfade**, retired models go through the retire queue and are deleted on
the worker, generation tags apply, and the app splits oversized blocks. This ADR adds only:
- The fade is aligned because of the fixed slot latency (D4).
- Swaps are allowed during playback (ADR-0004 §5, which answers NR open question 5).
- We do **not** inherit the research's "2× CPU for about 2 blocks" figure (NR §3). The fade lasts 25 ms (about
  19 blocks at 48 kHz/64), as costed by ADR-0004.

### D7. CPU budget
- **Per NAM instance: ≤ 8% of one core** at 48 kHz/64 for A1-Standard (steady state, normalised as in ADR-0007).
  During a fade, the ceiling is the steady-state chain plus one NAM instance (ADR-0004 Consequences). The gate for
  that case is owned by `requirements.md` (#40).
- v1 allows **one amp per preset**. The full chain stays under 25% of a core in **steady state** (AGENTS.md §4).
- AVX2 is decided by ADR-0006's `OSP_ENABLE_AVX2` after the Phase-3 benchmarks.

### D8. Default amps and captures
- **Default amp models are classic DSP** (`ClassicAmpModel`, dsp-core stages per ADR-0005). **No third-party `.nam`
  file ships** (NR §1.10, RA §1.3).
- NAM core `example_models` (MIT) are used **only in `tests/`**, with a row in `assets/LICENSES.md` (RA N1).
- Our own captures (NR open question 4, RA §2.3) are deferred: owner preset-designer with the dsp-engineer, after
  Phase 3.

### D9. A2 slimmable quality
- This is a **global app setting, "Amp CPU quality"** (0.0–1.0, default 1.0), not a preset parameter. It answers
  NR open question 3.
- `SetSlimmableSize()` allocates, and NAM core installs the new network inside `process()`, which would release the
  old one there. So quality is a **load option**: the control thread asks the worker to rebuild the model with
  `AmpLoadOptions{quality}`. The worker calls `SetSlimmableSize()` and then `prepare()`, so the staged network is
  consumed off the audio thread. The result is swapped through D6 with the 25 ms crossfade. No audio-thread call
  touches quality.

## Requirements (each maps to a test in `tests/`)

| ID | Requirement | Test |
|---|---|---|
| REQ-NAM-001 | Loading any NAM core `example_models` file at 0.5.0–0.7.0 succeeds. A file with version 0.8.0, 2 channels, or invalid JSON fails with `ModelLoadError` and a non-empty message. | Unit: loader fixtures |
| REQ-NAM-002 | At host rates **48 kHz (native) and 44.1 kHz (resampled)**, `process()` performs no allocation, lock, throw, I/O or system call while models are swapped 1000× under random block sizes **0…maxBlock**. The resampler error counter stays at 0. | RT checker (ADR-0007) + swap fuzz |
| REQ-NAM-003 | At host rate = model rate, the output (after removing the D4 padding) nulls against NAM core's direct `process()` to < −120 dBFS. | Unit: null test |
| REQ-NAM-004 | At 44.1/88.2/96 kHz, the slot's reported latency equals the measured impulse delay ± 1 sample, and a 1 kHz sine's level matches the 48 kHz render within ±0.5 dB. The same test decides whether native adaptation is exact at 96 kHz. | Unit: resampling |
| REQ-NAM-005 | A host block of 2 × maxBlockSize, split by the app, gives a result identical to two calls of maxBlockSize. | Unit: chunking |
| REQ-NAM-006 | Swapping a resampled NAM model for a classic amp (and back) at 44.1 kHz: the reported latency is unchanged, and under a 440 Hz sine max\|y[n] − y[n−1]\| does not exceed the larger steady-state value of the two models by more than 1 dB. The swap completes within 25 ms + 1 block. | Unit: swap |
| REQ-NAM-007 | Input trim, Normalized gain and Calibrated gain equal the D5 formulas to 0.01 dB for fixtures with and without metadata. | Unit: calibration |
| REQ-NAM-008 | `nam/a1-standard/48k/64` ≤ 8% of one core (normalised, steady state). | `osp-bench` gate |
| REQ-NAM-009 | No NAM/Eigen/nlohmann include is reachable from any header under `engine/include/`. | CI include check |
| REQ-NAM-010 | `process()` with n = 0, and `process()` before `prepare()`, are no-ops (`out` zero-filled). After `reset()`, two renders of the same input are bit-identical, and `reset()` does no model compute (it runs in < 5% of one block's budget). | Unit + RT checker |

## Consequences
- + There is one reference runtime with maximum `.nam` compatibility. The interface also serves classic amps and a
  possible faster backend.
- + The latency is constant per host rate and quality, so the host never sees a change at swap time and crossfades
  are aligned.
- + No NAM, resampler or slimmable code allocates, throws or prints on the audio thread.
- − At 44.1 kHz, every amp (including native or classic ones) carries the resampler latency, because of the padding.
- − Models trained at rates other than 48 kHz are rejected unless their latency fits the slot.
- − We maintain a small fork of the resampler.
- − Snapshot-based `reset()` may double the state memory per model (or need a second instance).
- − Changing quality rebuilds the model (it takes about a load time, then a crossfade).

## Open items, deferred with an owner
| Item | Owner | When |
|---|---|---|
| Native SR adaptation exactness (REQ-NAM-004) | dsp-engineer | First NAM ticket |
| Snapshot mechanism for `reset()` (state copy vs second instance) | dsp-engineer | First NAM ticket |
| NeuralAudio backend if REQ-NAM-008 fails | dsp-engineer (ADR-0002 amendment) | Phase 3 |
| Non-48 kHz-trained models (relax the rejection) | dsp-engineer | After user reports |
| `outputInterfaceLevelDbu` | audio-io-engineer | Phase 4 |
| Own `.nam` captures | preset-designer + dsp-engineer | After Phase 3 |
