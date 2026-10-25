# ADR-0002: NAM runtime and the `IAmpModel` interface

- Status: **Proposed** (2026-10-25). It becomes Accepted when the PR merges.
- Deciders: architect (acct1/Sable). Issue #26.
- Rests on: [`docs/research/nam-runtime.md`](../research/nam-runtime.md) (#6, cited as NR §x),
  [`docs/research/redistributable-assets.md`](../research/redistributable-assets.md) (RA §x),
  [`docs/research/oversampling.md`](../research/oversampling.md) (#10, OS §x), ADR-0001, and ADR-0006 (#28, in review:
  pins and the dependency list), ADR-0007 (#29, in review: benchmark normalisation).

## Context

The product is hybrid: amps use `.nam` neural models, and everything else is classic DSP (`docs/MASTER_PROMPT.md`).
The research found:
- NAM core v0.6.0 (MIT) is the reference runtime. It covers every `.nam` version from 0.5.0 to 0.7.0 and every
  architecture (WaveNet A1/A2, LSTM, ConvNet, Linear, SlimmableContainer) (NR §2.2–2.3).
- NeuralAudio (MIT) is faster but adds a second code path. RTNeural alone cannot run `.nam` WaveNet (NR §1.2–1.3).
- An A1-Standard model costs about 2.8% of a core at 48 kHz/64 in an optimised port. The C++ cost is *unverified*
  (NR §2.4).
- `get_dsp()`, `Reset()` and `prewarm()` allocate, and the reference plugin's model swap is not real-time safe
  (NR §2.2, §2.5).
- Models are mono, with zero latency at their native rate. Other rates need resampling or dilation scaling (NR §2.2, §2.5).
- No redistributable high-quality capture exists. The NAM core `example_models` (MIT) may be used in tests only
  (RA §1.3, N1).

## Decision

### D1. Runtime
- **v1 uses NAM core only**, pinned by ADR-0006 (v0.6.0, commit pinned there), linked `PRIVATE` into `osp_engine`.
  No NAM, Eigen or nlohmann type appears in a public `engine/` header.
- **NeuralAudio and RTNeural are not fetched in v1** (this answers NR open question 1). Re-open condition: the
  Phase-3 `osp-bench` case `nam/a1-standard/48k/64` exceeds the per-instance budget (D7). The owner is the
  dsp-engineer, who adds a second `IAmpModel` backend through an ADR-0002 amendment. No other code changes, because
  of D2.

### D2. `IAmpModel` (in `engine/`, no JUCE, no NAM types)

```cpp
namespace osp::engine {
struct AmpModelInfo {               // filled at load time, immutable afterwards
    std::string  architecture;      // "WaveNet", "LSTM", ...
    double       trainingSampleRate; // 48000 if the file has none (D4)
    std::optional<double> inputLevelDbu, outputLevelDbu, loudnessDb;
    bool         supportsQuality;   // slimmable (A2) models
};
class IAmpModel {
public:
    virtual ~IAmpModel() = default;
    // Non-RT (loader or message thread). May allocate. Called before the first process() and on any SR/maxBlock change.
    virtual void prepare(double hostSampleRate, int maxBlockSize) = 0;
    // RT-safe. Mono. numSamples <= maxBlockSize. in == out allowed.
    virtual void process(const float* in, float* out, int numSamples) noexcept = 0;
    // RT-safe. Clears internal history without allocating (e.g. on transport stop). May be implemented as a re-prewarm
    // into preallocated state, or as a flag that zeros buffers.
    virtual void reset() noexcept = 0;
    // RT-safe getter. Base-rate samples, an integer (resampler latency, 0 at native rate).
    virtual int  getLatencySamples() const noexcept = 0;
    // RT-safe only if every sub-model is preloaded (D9).
    virtual void setQuality(float q01) noexcept = 0;
    virtual const AmpModelInfo& info() const noexcept = 0;
};
// Non-RT factories. They throw osp::engine::ModelLoadError with a user-readable message.
std::unique_ptr<IAmpModel> loadNamModel(const std::filesystem::path&);
std::unique_ptr<IAmpModel> makeClassicAmp(ClassicAmpId);  // dsp-core based, see D8
}
```

Both implementations (`NamAmpModel` and `ClassicAmpModel`) go through the same handoff (D6), calibration (D5) and
tests. If NAM core's `Reset()` cannot provide an allocation-free `reset()`, `NamAmpModel::reset()` zeroes its
preallocated input history and runs `GetPrewarmSamples()` of silence through `process()` in chunks. The first DSP
ticket verifies that this does not allocate, using the RT checker from ADR-0007.

### D3. Accepted files and architectures
- `.nam` file versions 0.5.0 to 0.7.0, as accepted by NAM core's version checker. Anything else is rejected with
  "unsupported model version X" (NR §2.3).
- Any architecture NAM core loads is accepted, because the cost is ours only at runtime: A1 Standard/Lite/Feather/Nano,
  A2 Full/Lite (SlimmableContainer), LSTM, ConvNet and Linear. Benchmarks and goldens cover A1-Standard, A1-Nano,
  A2-Full, A2-Lite and LSTM 1×16.
- **Mono only:** `NumInputChannels() == 1 && NumOutputChannels() == 1`. Anything else is rejected (NR §2.5). Stereo
  starts after the amp (cab/FX).

### D4. Sample rate and block size
| Host rate vs model rate | Handling | Reported latency |
|---|---|---|
| Equal | Native | 0 |
| Host = 2× or 4× model, and NAM core reports `SupportsArbitrarySampleRate()` for that model | Native adaptation via `Reset(hostRate, …)` | 0 |
| Anything else (44.1 ↔ 48 kHz, 88.2 kHz, unsupported models) | AudioDSPTools `ResamplingContainer` (Lanczos, A = 12) around the model | Resampler latency, rounded up to an integer |

- A model without `sample_rate` is treated as 48 kHz (NR §2.3).
- NR open question 2 (which models support arbitrary rates) is deferred to the dsp-engineer's first NAM ticket.
  The test is `REQ-NAM-004` below, and the table's fallback is the default until then.
- Host blocks larger than the prepared `maxBlockSize` are split in `engine/` into chunks of at most `maxBlockSize`.
  `prepare()` is never called on the audio thread.
- NAM runs at its native rate, without oversampling (OS §3.1). A 2× "HQ" option is deferred to ADR-0005 (#27).

### D5. Level calibration
- Engine parameter `amp.inputInterfaceLevelDbu`: −60…+60 dBu, default **12 dBu**, not automatable (a rig setting,
  stored in app settings rather than presets). Input trim = `inputInterfaceLevelDbu − info.inputLevelDbu` when present,
  otherwise 0 dB (NR §2.5).
- Engine parameter `amp.outputMode` ∈ {Raw, Normalized, Calibrated}, per preset. Normalized applies
  `−18 dB − loudnessDb`, and Calibrated applies `outputLevelDbu − inputInterfaceLevelDbu` (NR §2.5).
  The default is **Normalized** when `loudnessDb` is present, Raw otherwise. If the selected mode's metadata is
  missing, Raw is used and the UI greys out that option.
- Gains are smoothed over 20 ms (`parameters.md` will own the exact value).

### D6. Loading and hot-swap (summary; `docs/spec/threading.md` will hold the detail)
1. **Loader thread:** `loadNamModel` → validate (D3) → `prepare(hostRate, maxBlock)` → prewarm. The result is a
   ready `IAmpModel`.
2. **Publish:** an atomic single-slot exchange (`std::atomic<IAmpModel*>`, release/acquire). A newer pending model
   replaces an unconsumed one, which the loader retires itself.
3. **Audio thread:** at block start it takes the pending model, crossfades from old to new over **32 ms** (linear
   equal-gain, both models running), then pushes the old pointer into an SPSC **retire queue**.
4. **GC/loader thread:** drains the retire queue and deletes models.
5. Swaps are **allowed during playback**, with the crossfade (this answers NR open question 5). The crossfade
   doubles the amp CPU for about 32 ms, so the budget in D7 includes that headroom.
6. On `prepare()` (SR/maxBlock change), the engine re-prepares the live models on the message thread while audio is
   stopped (host contract). No handoff is needed then.

### D7. CPU budget
- **Per NAM instance: ≤ 8% of one core** at 48 kHz/64 for A1-Standard, normalised as in ADR-0007, and ≤ 16% during
  a crossfade (NR §1.4).
- v1 allows **one amp instance per preset** (dual-amp is a later feature). The full chain stays < 25% (AGENTS.md §4).
- AVX2 is decided by ADR-0006's `OSP_ENABLE_AVX2` after the Phase-3 benchmarks.

### D8. Default amps and captures
- **Default amp models are classic DSP** (`ClassicAmpModel`, dsp-core waveshaper + tone-stack stages per
  ADR-0005/#27). **No third-party `.nam` file ships** (NR §1.10, RA §1.3).
- NAM core `example_models` (MIT) are used **only in `tests/`**, with a row in `assets/LICENSES.md` (RA N1).
- Our own captures (NR open question 4, RA §2.3) are deferred to a later ticket: "Own NAM captures trained from
  white-box/SPICE renders". Owner: preset-designer with the dsp-engineer, after Phase 3. They ship only with a row in
  `assets/LICENSES.md`.

### D9. A2 slimmable quality
- This is a **global app setting, "Amp CPU quality"** (0.0–1.0, default 1.0), not a preset parameter, so a preset
  sounds the same on every machine unless the user opts in. It answers NR open question 3.
- All sub-models are preloaded at load time, so `setQuality()` is RT-safe (NR §2.5). This costs memory per A2 model
  (*unverified* size, to be measured in the first NAM ticket).

## Requirements (each maps to a test in `tests/`)

| ID | Requirement | Test |
|---|---|---|
| REQ-NAM-001 | Loading any NAM core `example_models` file at 0.5.0–0.7.0 succeeds. A file with version 0.8.0, 2 channels, or invalid JSON fails with `ModelLoadError` and a non-empty message. | Unit: loader fixtures |
| REQ-NAM-002 | `process()` performs no allocation, lock or system call while models are swapped 1000× under random block sizes 1…maxBlock. | RT checker (ADR-0007) + swap fuzz |
| REQ-NAM-003 | At host rate = model rate, the latency is 0, and the output nulls against NAM core's direct `process()` to < −120 dBFS. | Unit: null test |
| REQ-NAM-004 | At 44.1/88.2/96 kHz host rate, `getLatencySamples()` equals the measured impulse delay ± 1 sample, and a 1 kHz sine's level matches the 48 kHz render within ±0.5 dB. | Unit: resampling |
| REQ-NAM-005 | A host block of 2 × maxBlockSize gives a result identical to two calls of maxBlockSize. | Unit: chunking |
| REQ-NAM-006 | During a swap under a 440 Hz sine, max\|y[n] − y[n−1]\| does not exceed the larger steady-state value of the old and new model by more than 1 dB (no click). The swap completes within 32 ms + 1 block. | Unit: swap click |
| REQ-NAM-007 | Input trim, Normalized gain and Calibrated gain equal the D5 formulas to 0.01 dB for fixtures with and without metadata. | Unit: calibration |
| REQ-NAM-008 | `nam/a1-standard/48k/64` ≤ 8% of one core (normalised). A crossfade case is ≤ 16%. | `osp-bench` gate |
| REQ-NAM-009 | No NAM/Eigen/nlohmann include is reachable from any header under `engine/include/`. | CI include check |

## Consequences
- + There is one reference runtime with maximum `.nam` compatibility. The interface leaves room for a faster
  backend or our own classic amps without touching the graph.
- + The handoff design fixes the reference plugin's audio-thread deallocation and race.
- − Without NeuralAudio's static kernels, CPU may be higher than the optimised-port numbers. That risk is gated by
  REQ-NAM-008 and the D1 re-open condition.
- − The resampler adds latency at 44.1 kHz with 48 kHz models (the common Windows default). It is reported to the host.
- − Preloading slimmable sub-models increases memory use.
- − Shipping no captures means the out-of-box amp tone depends on our classic-DSP amps (ADR-0005/#27 and the amp
  tickets).

## Open items, deferred with an owner
| Item | Owner | When |
|---|---|---|
| Which models support arbitrary SR (REQ-NAM-004 decides native vs resample at 96 kHz) | dsp-engineer | First NAM ticket |
| NeuralAudio backend if REQ-NAM-008 fails | dsp-engineer (ADR-0002 amendment) | Phase 3 |
| Own `.nam` captures | preset-designer + dsp-engineer | After Phase 3 |
| Exact thread and queue types | architect | `docs/spec/threading.md` |
