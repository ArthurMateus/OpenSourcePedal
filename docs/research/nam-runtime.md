# NAM / RTNeural runtime integration and CPU cost

- Issue: #6
- Author: acct1/researcher (Sable)
- Date: 2026-10-25 (all sources accessed on that date unless stated otherwise)
- Status: research note. The architect turns it into an ADR and `docs/spec/`.

## 1. Summary

1. **Runtime: use NeuralAmpModelerCore (NAM core) v0.6.0, MIT**, pinned by tag and fetched at configure time. It is the
   reference implementation, it loads every `.nam` version from 0.5.0 to 0.7.0, and it runs WaveNet (A1 and A2),
   LSTM, ConvNet, Linear and slimmable containers [S1–S4].
2. **Put it behind our own `engine/` interface** (`IAmpModel`) so we can swap in **NeuralAudio** (MIT) later. NeuralAudio
   wraps NAM core and RTNeural, adds static fast paths for the standard A1 sizes and LSTM sizes, and handles calibration
   [S6]. That makes it the main alternative if NAM core misses the CPU budget in Phase 3.
3. **RTNeural (BSD-3-Clause) alone is not enough.** It runs LSTM/GRU/Conv1D/Dense keras-or-torch models, but it does not
   load `.nam` WaveNet files. The NAM WaveNet port (RTNeural-NAM) is still experimental and lacks gated activations
   [S7, S8]. Use it only through NeuralAudio, and only for LSTM.
4. **CPU fits the budget.** A1-Standard costs about **37 µs per 64-sample block at 48 kHz** (2.8% of one core) in an
   optimised port on a Ryzen 7 5700U. A2-Full costs 27 µs (2.0%) and Nano 17 µs (1.3%) [S9]. No equally clean number
   for NAM core C++ is published (*unverified*). Plan for **≤ 8% of a core per amp instance** until our own `osp-bench`
   measures it. That leaves about 17% of the 25% budget for everything else.
5. **NAM models are mono, with zero algorithmic latency at their native rate.** Latency only appears when we resample.
   Report that resampler latency through `setLatencySamples` [S5, S10].
6. **Handle sample rate as follows.** At 48 kHz, run models natively. At 96 kHz (an integer multiple), check whether NAM
   core v0.6.0 `SupportsArbitrarySampleRate()` covers the model; if not, use NeuralAudio's dilation scaling [S2, S6].
   At 44.1 or 88.2 kHz, wrap the model in a Lanczos resampler as the reference plugin does (AudioDSPTools, MIT, A = 12)
   [S5, S10].
7. **Load and swap models off the audio thread.** `get_dsp()`, `Reset()` and `prewarm()` all allocate [S2], so they run on
   a loader thread. Hand the model to the audio thread with a lock-free pointer handoff, and return the old model to a
   GC thread for deletion. **Do not copy the reference plugin here:** it moves `unique_ptr`s across threads without
   synchronisation and frees the old model on the audio thread (§2.5).
8. **Calibrate levels from `.nam` metadata:** `input_level_dbu`, `output_level_dbu` and `loudness` [S3, S5]. Offer Raw,
   Normalized (−18 dB target) and Calibrated output modes, and default the interface level to 12 dBu [S5, S6].
9. **Support** A1 Standard/Lite/Feather/Nano, A2 Full/Lite (including slimmable), and LSTM. Reject multi-channel models,
   as the reference plugin does [S5].
10. **Default captures:** we cannot ship third-party `.nam` files unless the file itself carries a redistributable licence.
    Most TONE3000 captures use a "T3K" licence that forbids redistribution [S12]. Plan to train our **own** captures
    with the NAM trainer (MIT) [S11] (see §4).

## 2. Findings

### 2.1 Libraries and licences

| Library | Version (2026-10-25) | Licence | AGPLv3-compatible? | Role | Source |
|---|---|---|---|---|---|
| NeuralAmpModelerCore | v0.6.0 (2026-10-01) | MIT | Yes | `.nam` loader + inference | S1 |
| Eigen (NAM core submodule) | submodule pin | MPL-2.0 (Eigen's stated licence; *unverified for this pin*) | Yes | Linear algebra | S1, S13 |
| nlohmann/json (vendored) | single_include | MIT | Yes | `.nam` JSON parsing | S1 |
| AudioDSPTools | main | MIT (resampler derived from iPlug2/WDL, licence text copied in header) | Yes | Lanczos resampler | S10 |
| NeuralAudio | v0.1.3 (2026-07-18) | MIT (+ deps in CREDITS.md) | Yes | Optimised NAM/LSTM runtime | S6 |
| RTNeural | main (last push 2026-08-20) | BSD-3-Clause | Yes | LSTM/GRU/Conv1D inference | S7 |
| RTNeural-NAM | experimental | BSD-3-Clause | Yes | WaveNet on RTNeural (TODO: gated activations) | S8 |
| NeuralAmpModelerPlugin | v0.7.15 | MIT | Yes (reference only) | iPlug2 reference integration | S5 |
| nam-juce | v0.5.0 | GPL-3.0 | Yes (GPLv3 → AGPLv3 OK) | JUCE reference integration | S14 |
| neural-amp-modeler (trainer) | v0.13.0 | MIT | Yes (tooling) | Training our own captures | S11 |

Each one we vendor or fetch goes into `THIRD_PARTY.md` (AGENTS.md rule 9).

### 2.2 NAM core API (v0.6.0)

| Topic | Finding | Source |
|---|---|---|
| Load | `nam::get_dsp(std::filesystem::path)` returns `std::unique_ptr<nam::DSP>`. It throws on invalid files (`NamFileValidationError`, `std::runtime_error`). | S2, S3 |
| Process | `DSP::process(NAM_SAMPLE** in, NAM_SAMPLE** out, int num_frames)`. It takes multichannel pointers, but factory models are 1-in/1-out. | S2, S5 |
| Reset | `Reset(sampleRate, maxBufferSize)` "may allocate and must be called outside real-time audio processing". It calls `prewarm()` by default (`SetPrewarmOnReset`). | S2 |
| Prewarm | `GetPrewarmSamples()` sets how long the model runs on silence to settle its state. | S2 |
| Sample rate | `GetExpectedSampleRate()` returns the training rate, or −1 if unknown. `SupportsArbitrarySampleRate()` defaults to `false`, and only some models override it. Which architectures do is *unverified*. | S2 |
| Metadata | `HasInputLevel/GetInputLevel` gives dBu RMS for a 0 dBFS-peak 1 kHz sine. Matching calls exist for `OutputLevel`. `GetLoudness()` gives dB and **throws** if absent, so check `HasLoudness()` first. | S2 |
| Thread safety | No thread-safety guarantees are documented. Treat one instance as single-threaded. Slimmable WaveNet changes its size through an internal atomic staging slot that `process()` picks up. | S2, S4 |
| Build | C++20, CMake ≥ 3.10, option `NAM_ENABLE_A2_FAST` (default ON). | S1 |
| Sharp edge | Eigen members can be misaligned with some compilers and flags. The workaround is `EIGEN_MAX_ALIGN_BYTES 0` + `EIGEN_DONT_VECTORIZE`, which costs performance (core issue #67). | S1 |

### 2.3 `.nam` file format

The file is JSON with these top-level keys: `version` (semver), `architecture`, `config`, `weights`, `sample_rate`, and
`metadata` (which may contain `loudness`, `input_level_dbu` and `output_level_dbu`) [S3]. Core v0.6.0 accepts file
versions **0.5.0 to 0.7.0**. A newer major or minor version is rejected through a pluggable version-support checker [S3].
Registered architecture names are `WaveNet`, `LSTM`, `ConvNet`, `Linear` and `SlimmableContainer` [S4].
Older files may lack `sample_rate`. The reference plugin then assumes 48 kHz [S5].

### 2.4 Architectures and CPU cost

| Architecture | Channels | µs per 64 samples at 48 kHz | % of one core | Notes | Source |
|---|---|---|---|---|---|
| A1 WaveNet Standard | 16 | 36.9 | 2.8% | Highest fidelity, heaviest A1 | S9 |
| A1 WaveNet Lite | 12 | 52.2 | 3.9% | Slower than Standard in this port (kernel-specific) | S9 |
| A1 WaveNet Feather | 8 | 19.3 | 1.4% | | S9 |
| A1 WaveNet Nano | 4 | 17.2 | 1.3% | Per-layer overhead dominates | S9 |
| A2-Full | 8 | 27.3 | 2.0% | Vendor claims 30–40% less CPU than A1 | S9, S15 |
| A2-Lite | 3 | 18.4 | 1.4% | Aimed at embedded hardware | S9, S15 |
| LSTM 1×16 / 2×8 | n/a | 7.5 | 0.6% | | S9 |

Caveats:
- The numbers come from **NeuralAmpModeler-rs**, a Rust port, on a Ryzen 7 5700U (AVX2) [S9]. The C++ NAM core
  is likely slower before NeuralAudio-style static kernels are applied. The port's own pre-optimisation baseline for
  Standard was about 93 µs (7%) [S9]. Treat 3–7% per A1-Standard instance as the expected range (*unverified* for C++).
- Embedded reports (Nano 36–39%, Feather 66–69% on an ARM pedal at 128 frames) show how steep the scaling is.
  They do not apply to desktop [S16].
- NeuralAudio needs `/arch:AVX2` and `-DMULTIFRAME_8X8_CONVOLUTION=8` on x64 for its best performance. A
  misconfigured build is *slower* [S6]. Its `WAVENET_FRAMES` default is 64, which matches our reference block size [S6].

### 2.5 How existing plugins integrate NAM

NeuralAmpModelerPlugin v0.7.15 (iPlug2) [S5]:
- **Load:** on the UI thread. `_StageModel()` calls `get_dsp`, checks for 1-in/1-out, wraps the model in `ResamplingNAM`,
  calls `Reset(sr, block)`, optionally sets the slimmable size, and stores the result in `mStagedModel`.
- **Swap:** at the start of `ProcessBlock`, `_ApplyDSPStaging()` runs `mModel = std::move(mStagedModel)`.
  **Problems** (from reading the code): there is no atomic or fence between the UI-thread write and the audio-thread
  read, and the assignment destroys the previous model **on the audio thread**, which deallocates there. Both break our
  real-time rules.
- **Resampling:** `ResamplingNAM` wraps the model in AudioDSPTools' `ResamplingContainer` (Lanczos, A = 12) when the
  host rate differs from the model rate. `GetLatency()` returns the resampler latency, which goes to the host through
  `SetLatency` [S5, S10].
- **Calibration:** input gain gets `+ (InputCalibrationLevel − model input_level_dbu)`, defaulting to 12 dBu. Output
  mode "Normalized" applies `−18 − loudness` and "Calibrated" applies `output_level_dbu − InputCalibrationLevel` [S5].
- **Slimmable A2:** a `Slim` parameter maps to `SetSlimmableSize()` [S5]. NeuralAudio exposes the same idea as a
  quality scale from 0.0 to 1.0. It is real-time safe only when all sub-models are preloaded (the default) [S6].

NeuralAudio [S6]: `NeuralModelLoader::CreateFromFile`, `model->Process(in, out, n)`. `n` must stay at or below
`SetMaxAudioBufferSize`, which defaults to 128 and is not real-time safe to change. It provides
`GetRecommendedInput/OutputDBAdjustment()` and an external sample rate that rescales WaveNet dilations, applied at
load time and only for integer multiples. It also reports the receptive field.

## 3. Recommendations

| Decision | Recommendation |
|---|---|
| Runtime | NAM core **v0.6.0** (MIT) through `cmake/` FetchContent, pinned to the tag SHA, with Eigen from its submodule. Hide it behind `engine/` `IAmpModel` (no NAM types in public headers). |
| Fallback runtime | NeuralAudio (MIT) as a second `IAmpModel` backend, if Phase-3 benchmarks show NAM core above 8% per instance. |
| RTNeural | Do not integrate directly in v1. It comes in transitively through NeuralAudio if we adopt that. |
| Architectures | A1 Std/Lite/Feather/Nano, A2 Full/Lite (+ slimmable), LSTM. Mono 1-in/1-out only. Reject other channel counts with a user-visible error. |
| Sample rate | Native at 48 kHz. 96 kHz uses native arbitrary-SR support if available, otherwise a 2× resample. 44.1/88.2 kHz use the Lanczos resampler (A = 12). Add the resampler delay to `setLatencySamples`. Missing `sample_rate` means 48 kHz. |
| Block size | Call `Reset(sr, maxBlock)` from `prepareToPlay`. On the audio thread, split host blocks larger than `maxBlock` into chunks of at most `maxBlock`, so we never re-prepare there. |
| Loading | Loader thread: `get_dsp` → validate → wrap → `Reset` → prewarm → publish with a lock-free single-slot exchange (`std::atomic<IAmpModel*>`). The audio thread takes the model at block start and pushes the old one to an SPSC "retire" queue, which a GC thread drains. |
| Swap click | Crossfade old→new over 20–30 ms. Both models run during the fade, so budget 2× CPU for about 2 blocks of 64 samples. |
| Calibration | Parameter `inputInterfaceLevel_dBu`, default 12, range −60…+60 (as in the reference plugin). Output modes Raw / Normalized (−18 dB) / Calibrated, with the default Normalized when `loudness` is present, Raw otherwise. |
| RT hygiene | FTZ/DAZ on. Use the same Eigen alignment macros in every translation unit. Fuzz test: load random factory models while processing, and assert no allocation on the audio thread with our debug checker. |
| Benchmarks | Add an `osp-bench` case per architecture at 48 kHz / 64 and 44.1 kHz / 32 (resampled) with `/arch:AVX2`. Gate: ≤ 8% of a core per instance. |
| Default captures | Ship **no** third-party `.nam` files. Train our own captures of our own white-box amp blocks and/or owner-owned hardware with the MIT trainer, and record them in `assets/LICENSES.md`. Only CC0 or CC-BY captures with a stated licence may be added, each with a row. |

## 4. Open questions (for the architect)

1. Should we start with NAM core only, or wire NeuralAudio from day one (more dependencies, faster)? This note suggests
   NAM core first, with the interface ready for both.
2. Which A1/A2 architectures does `SupportsArbitrarySampleRate()` actually cover in v0.6.0? Test it in Phase 3 and decide
   between native-rate adaptation and resampling at 96 kHz.
3. Should A2 slimmable "quality" be a user parameter, or an automatic CPU-saver tied to a global quality setting?
4. What is the source of our default captures: training NAM on our own DSP amp models (a circular but licence-clean
   route), or capturing hardware the owner owns? This decision needs an ADR.
5. What is the crossfade length on model swap, and do we allow swaps during playback or only when transport is stopped
   (in the plugin)?

## 5. Sources (all accessed 2026-10-25)

- S1: NeuralAmpModelerCore repo, README, CMakeLists.txt, .gitmodules, LICENSE (MIT), tag v0.6.0. https://github.com/sdatkinson/NeuralAmpModelerCore
- S2: NAM core `NAM/dsp.h` @ v0.6.0. https://github.com/sdatkinson/NeuralAmpModelerCore/blob/v0.6.0/NAM/dsp.h
- S3: NAM core `NAM/get_dsp.h`, `NAM/get_dsp.cpp`, `NAM/nam_file.h` @ v0.6.0. https://github.com/sdatkinson/NeuralAmpModelerCore/tree/v0.6.0/NAM
- S4: NAM core architecture registrations (`wavenet/model.cpp`, `lstm.cpp`, `convnet.cpp`, `linear.cpp`, `container.cpp`, `wavenet/slimmable.h`) @ v0.6.0. https://github.com/sdatkinson/NeuralAmpModelerCore/tree/v0.6.0/NAM
- S5: NeuralAmpModelerPlugin `NeuralAmpModeler/NeuralAmpModeler.cpp` and `.h` (main). https://github.com/sdatkinson/NeuralAmpModelerPlugin/tree/main/NeuralAmpModeler
- S6: NeuralAudio README and LICENSE (MIT), v0.1.3. https://github.com/mikeoliphant/NeuralAudio
- S7: RTNeural README and LICENSE (BSD-3-Clause). https://github.com/jatinchowdhury18/RTNeural
- S8: RTNeural-NAM README. https://github.com/jatinchowdhury18/RTNeural-NAM
- S9: NeuralAmpModeler-rs 0.1.0 benchmarks. https://docs.rs/crate/NeuralAmpModeler-rs/0.1.0/source/docs/benchmarks.md
- S10: AudioDSPTools `dsp/ResamplingContainer/ResamplingContainer.h` (MIT). https://github.com/sdatkinson/AudioDSPTools
- S11: neural-amp-modeler trainer, v0.13.0 (MIT). https://github.com/sdatkinson/neural-amp-modeler
- S12: TONE3000 Tone Sharing Policy and per-tone licence text. https://www.tone3000.com/policy
- S13: Eigen licensing (MPL-2.0). https://eigen.tuxfamily.org/index.php?title=Main_Page#License (*not re-fetched; from prior knowledge, verify when vendoring*)
- S14: nam-juce, v0.5.0 (GPL-3.0). https://github.com/Tr3m/nam-juce
- S15: TONE3000 blog, "Introducing NAM Architecture 2 (A2)", 2026-06-02. https://tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2
- S16: MOD forum thread "Neural Amp Modeler" (embedded CPU reports). https://forum.mod.audio/t/neural-amp-modeler/10112?page=3
