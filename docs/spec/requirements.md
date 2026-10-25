# Requirements

- Status: draft spec (issue #21). Date: 2026-10-25. Owner: architect (acct3/Moth).
- Inputs: `docs/MASTER_PROMPT.md` §1 and §5, AGENTS.md §4, and the research notes cited per row (short names below).
  Related specs: `threading.md`, `realtime-rules.md` (#25); ADR-0006 (build); `signal-flow.md` (#22),
  `parameters.md` (#23), preset schema (#24), oversampling ADR (#27), test strategy ADR (#29).

## 0. Conventions

- **IDs** `REQ-NNN`, grouped by hundreds. IDs are never reused. A dropped requirement is struck through, not deleted.
- **Priority:** **M** = must for v1.0; **S** = should for v1.0 (may slip to v1.x with an issue); **P** = post-v1 backlog.
- **Verify** (the test type that proves it; #29 turns these into CI jobs):
  **U** unit test (Catch2) · **G** golden render · **RT** RT checker (realtime-rules.md §2) · **B** benchmark
  (`osp-bench`) · **PV** pluginval · **I** automated integration test (app or engine level, no hardware) ·
  **H** hardware check on the reference machine with a written procedure and recorded result · **R** static check
  (guard script, review checklist) · **D** document inspection.
- Source short names: `asio` = asio-low-latency, `fw` = framework-comparison, `nam` = nam-runtime, `os` = oversampling,
  `tt` = test-tooling, `ref` = reference-plugins, `ta`/`tb` = tone-targets-a/-b, `tbe` = time-based-effects,
  `assets` = redistributable-assets, `pa` = prior-art, `MP` = MASTER_PROMPT.

## 1. Platform and deliverables (1xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-101 | Ship a Windows 10 (1607+) / 11 x64 **standalone app** and a **VST3** plugin built from one code base | M | I (CI artefacts) | MP §1; fw §2.1 |
| REQ-102 | `dsp-core/` compiles without JUCE and without OS headers (`OSP_BUILD_APP=OFF` build succeeds) | M | R, I | ADR-0001; ADR-0006 D3–D4 |
| REQ-103 | Internal processing is 32-bit float. Mono from the input through the cab; stereo from the cab output on (`signal-flow.md`) | M | U | MP §1; tbe Q1 |
| REQ-104 | CLAP and AU formats are not part of v1 | P | D | fw Q1 |

## 2. Audio I/O (standalone) (2xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-201 | Device types offered in this order: ASIO (if a driver is present), WASAPI Low Latency, WASAPI Exclusive, WASAPI shared | M | I | asio R5 |
| REQ-202 | Sample rates 44.1, 48, 88.2 and 96 kHz. The default is 48 kHz | M | U, I | MP §1; asio R4 |
| REQ-203 | Buffer sizes 32, 64, 96, 128, 192, 256, 512, 1024 and 2048 samples (as far as the driver allows). The default is 64 | M | U, H | MP §1; asio R4 |
| REQ-204 | Changing the device, sample rate or buffer size while running causes no crash, no NaN, and recovers audio within 1 s | M | I, H | asio §2.4 |
| REQ-205 | Device unplug/replug is detected; the app shows the state and resumes when the device returns | S | H | MP gate 7 |
| REQ-206 | Exclusive mode shows a notice that other applications are silenced on that device | S | I | asio §2.4 |
| REQ-207 | The input channel (mono L/R) and output pair are selectable. The selection is a global setting, not part of a preset | M | I | ref §1.8 |
| REQ-208 | The standalone process opts out of EcoQoS and never calls `timeBeginPeriod` | M | R, I | asio R8 |

## 3. Latency (3xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-301 | The plugin reports total processing latency (oversampling, ADAA, resamplers, linear-phase filters) via `setLatencySamples`. An impulse test confirms the reported value is within ±1 sample at 1 kHz group delay | M | U, I | os §3.4; AGENTS §4 |
| REQ-302 | Processing latency in the default ("Live") quality is ≤ 16 samples at 48 kHz (*unverified* target, confirmed by the #27 ADR) | S | U | os §1.8 |
| REQ-303 | Enabling or bypassing a block never changes the reported latency (constant latency per quality mode) | M | U | tbe Q2 |
| REQ-304 | The UI shows driver input latency, driver output latency, processing latency, their sum, and the **measured** round-trip latency (samples and ms) | M | I, H | MP §1; asio R10 |
| REQ-305 | A loopback **Measure round-trip** tool measures RTL to ±1 sample at SNR ≥ 20 dB and stores the offset per device + buffer size | M | U (offline detector), H | asio R10–R11 |

## 4. Signal chain and blocks (4xx)

Order and routing are specified in `signal-flow.md` (#22). Parameters, ranges and defaults are in `parameters.md` (#23).

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-401 | Chain: Input → Gate → Comp → Drive A → Drive B → Amp → Cab → EQ → Modulation → Delay → Reverb → Doubler → Output, plus a Tuner tap on the input | M | U, G | MP §1; ref R1 |
| REQ-402 | Every block has a bypass. Hard bypass nulls against its input to ≤ −120 dBFS RMS after latency alignment | M | U | AGENTS §4; tt §2.2 |
| REQ-403 | **Noise gate:** threshold, hysteresis, attack, hold, release, range; keyed from the pre-boost input | M | U | tb §1.4 |
| REQ-404 | **Compressor:** threshold, ratio, attack, release, makeup, blend (parallel), plus a dual-stage mode | M | U | ta §1.2 |
| REQ-405 | **Drive A/B:** selectable voicings: tight-boost (pre-HPF ≈ 720 Hz in the clip loop), low-gain transparent OD, distortion, fuzz. The attack (pre-HPF) selector has 6 positions | M | U, G | tb §1.2; ta §1.5 |
| REQ-406 | **Amp:** 3 channels (Clean, Crunch, Lead) stored in one preset. Switching channels is a parameter (no reload) and uses the ADR-0004 crossfade | M | U, I | ref Q5; ADR-0004 §5 |
| REQ-407 | Each amp channel is either a **classic-DSP amp model** (the default in factory presets) or a user **`.nam` model** | M | U, G | MP §1; assets §1.3 |
| REQ-408 | Amp channel controls: Gain, Bass, Mid, Treble, Presence, Master, Level (tone-neutral), Bright and Tight switches, and a pre-amp HPF 20–300 Hz | M | U | ref R2; tb §1.3 |
| REQ-409 | An amp-slot **"console"** type (tube saturation + comp + EQ + HPF/LPF) auto-bypasses the cab | S | U | ta §1.3 |
| REQ-410 | **NAM loading:** A1 Standard/Lite/Feather/Nano, A2 Full/Lite (incl. slimmable), and LSTM `.nam` versions 0.5.0–0.7.0. Multi-channel or invalid files are rejected with a user-visible error and no crash | M | U, I | nam §3 |
| REQ-411 | NAM calibration: input interface level (dBu, default 12; a **global setting**, ADR-0003 §7) and output mode Raw / Normalized (−18 dB) / Calibrated from file metadata (per preset) | M | U | nam §3; ADR-0003 |
| REQ-412 | NAM models run at their native rate. Other host rates use the Lanczos resampler (A = 12), with its latency reported | M | U | nam §3; os §1.6 |
| REQ-413 | **Cab:** 2 IR slots, each with IR (factory or user WAV/AIFF, any rate, resampled on load), Level −∞…+6 dB, Pan, Phase invert and On. Loading happens off the audio thread | M | U, RT | ref R3; ADR-0004 §4 |
| REQ-414 | **EQ:** 9-band graphic (63 Hz–16 kHz octave centres, ±12 dB) + HPF 20–250 Hz + LPF 2–17 kHz, and a 4-band parametric mode | M | U | ref R4 |
| REQ-415 | **Modulation:** chorus, flanger, phaser, vibe, tremolo, with a position switch **pre-amp / post-cab** (one block). This answers ref Q1 | M | U | ref Q1; tbe §1.5 |
| REQ-416 | **Delay:** digital / analog (BBD) / tape modes, 1–2000 ms free time (`delay.time_ms`), tempo sync, tap tempo, ping-pong, feedback low/high cut, ducking | M | U, G | tbe §1.3; ref R5 |
| REQ-417 | **Reverb:** room, hall, plate and shimmer ("dreamscape"); decay up to 8.5 s; HPF 50–700 Hz; LPF 1–10 kHz | M | U, G | tbe §1.1–1.2; ref R5 |
| REQ-418 | **Doubler:** stereo double-tracking from a short modulated delay, with zero added latency | S | U | ref R6 |
| REQ-419 | **Tuner:** chromatic, ±1 cent accuracy for 40–1000 Hz sine input; full page + always-visible strip; mutes output while open (option) | M | U, I | ref R6 |
| REQ-420 | **Transpose ±12 st**, polyphonic octaver, whammy, harmonizer, wah, envelope filter, amp blend mode, DI blend | P | — | ref R8, Q2–Q4; tb §1.1 |
| REQ-421 | Oversampling quality is a **global** Live/HQ setting. Offline renders (host non-realtime flag, `osp-render`) use HQ. Factors and filters are defined by the oversampling ADR (#27) | M | U | os Q1 |
| REQ-422 | Aliasing per nonlinear block meets the ASR thresholds of the oversampling ADR (#27), measured with the bin-centred method | M | U | os §3.3; tt §2.3 |
| REQ-423 | Continuous parameters are smoothed: no zipper noise per ADR-0007 REQ-TEST-003 (a 50 ms min→max ramp on a 1 kHz sine gives spectral splatter outside f0 ± 50 Hz ≤ −60 dBc) | M | U | AGENTS §4; ADR-0007; realtime-rules RT-13 |

## 5. Presets (5xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-501 | Presets are versioned JSON validated against `presets/schema/preset.v1.schema.json`. Older versions migrate forward automatically, and newer major versions are rejected with a message | M | U | MP §1; #24 |
| REQ-502 | User presets: create, save, save-as, rename, duplicate, delete, import and export (single file and bank). Folders supported. Dirty marker shown | M | I | MP §1; ref §1.8 |
| REQ-503 | A preset stores all block parameters (including the Global-Lock blocks `input`, `gate`, `transpose`, `doubler`, `output`), all 3 amp channels, and IR/`.nam` references (relative path + SHA-256 fallback). It never stores device settings, input mode, tuner, metronome, MIDI map, Global Lock on/off, UI scale or the NAM interface calibration level. With Global Lock on, loading a preset **skips applying** the Global-Lock blocks | M | U | ref R7, Q6; ADR-0003 §7 |
| REQ-504 | Global settings (device, input mode, Global Lock on/off, MIDI map and program-change map, UI scale, NAM interface level, quality) live in a separate settings file | M | U | ref Q6; ADR-0003 §7 |
| REQ-505 | Factory bank: **≥ 3 presets per inspiration** covered by tone-targets-a/-b, with original names only | M | R (guard), D | MP §1; ta, tb |
| REQ-506 | Every factory preset renders at −18 LUFS-I ± 0.5 LU (leads −17) with true peak ≤ −1 dBTP on the reference DI | M | G | tt §3; tb §1.4b |
| REQ-507 | Every factory preset matches its tone-target octave-band spectrum within 1.5 dB RMS (63 Hz–8 kHz, relative to 1 kHz) | S | G | tt §2.2; ta §1.6 |
| REQ-508 | Preset switching never glitches audio: the ADR-0004 crossfade, no allocation on the audio thread | M | RT, I | ADR-0004 §4–5 |

## 6. User interface (6xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-601 | Pedalboard view with section tabs, an amp page, an EQ/cab page, a preset browser, the tuner, I/O meters and the latency panel | M | I, D | MP §4; ref §2.4 |
| REQ-602 | Resizable window, 75–200 % scale, crisp at 100/150/200 % Windows display scaling | M | I, H | ref R6; fw §2.1 |
| REQ-603 | All artwork is original vector graphics. No third-party logos, trademarks or artist names | M | R, D | AGENTS rule 2 |
| REQ-604 | Knobs: drag, double-click to reset, Ctrl for fine control, typed value entry. Right-click or double-click a section to bypass it | S | I | ref §2.4 |
| REQ-605 | Peak meters with clip LEDs that hold for 10 s (click to clear) on input and output | M | I | ref §2.3 |
| REQ-606 | MIDI learn (CC/PC/Note, absolute/relative) and Global Lock | S | I | ref R6 |
| REQ-607 | Metronome (standalone only), Undo/Redo, Preset Explorer tags/favourites | P | — | ref R6 |
| REQ-608 | Errors (bad `.nam`/IR, device failure) appear as non-modal messages. The audio thread never waits on the UI | M | I | ADR-0004 |
| REQ-609 | About/Credits screen with the AGPL notice, a link to the corresponding source, and the attribution of every bundled CC BY asset | M | D | ADR-0001; assets §1.5 |

## 7. Performance (7xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-701 | Full chain (one amp channel active, all blocks on, Live quality) at 48 kHz / 64 samples uses **< 25 %** of one core, normalised to the reference kernel | M | B | MP §5; tt §2.6 |
| REQ-702 | A NAM amp instance (A1 Standard) uses ≤ 8 % of one core at 48 kHz / 64 | M | B | nam §1.4 |
| REQ-703 | During an ADR-0004 swap crossfade, CPU ≤ steady state + one NAM instance | M | B | ADR-0004 Consequences |
| REQ-704 | CI fails on a > 10 % normalised regression per block or per preset | M | B | AGENTS §4; tt §2.6 |
| REQ-705 | Zero xruns in the 10-minute soak test at 48 kHz / 64 on the reference machine (report-only on CI runners) | M | H, I | realtime-rules C7 |

## 8. Real-time safety and correctness (8xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-801 | All rules RT-1…RT-14 of `realtime-rules.md` hold for every audio-path function | M | RT, R | ADR-0004 |
| REQ-802 | Each block passes at every sample rate {44.1, 48, 88.2, 96} kHz × block size {0, 1, 32, 64, 127, 512, 2048}. The app additionally handles 2 × maxBlock by splitting | M | U, RT | tt §3; realtime-rules C8 |
| REQ-803 | Parameter fuzzing (random values every block for 60 s per block) produces no NaN/Inf/subnormal output | M | U | AGENTS §4 |
| REQ-804 | Model/IR/preset load fuzzing during processing (swap every 2 s) gives zero RT-checker violations | M | RT | nam §3 RT hygiene |
| REQ-805 | Golden renders: deterministic blocks null ≤ −120 dBFS RMS (≤ −110 peak) against stored goldens; neural and cross-platform paths stay within LSD mean ≤ 0.2 dB / max ≤ 1.0 dB | M | G | tt §2.2 |
| REQ-806 | Per-block frequency response, THD (997 Hz coherent) and gain staging are tested against the block spec | M | U | tt §2.3 |
| REQ-807 | `reset()` returns a block to its initial state: two renders separated by `reset()` are bit-identical | M | U | architect.md principles |

## 9. Plugin validation, legal and build (9xx)

| ID | Requirement | Pri | Verify | Source |
|---|---|---|---|---|
| REQ-901 | The VST3 passes pluginval at strictness ≥ 8 (CI level set by #29; 9 recommended) on every PR | M | PV | AGENTS §4; tt §2.5 |
| REQ-902 | State save/load round-trip: `getStateInformation` → `setStateInformation` → `getStateInformation` is byte-identical | M | U, PV | ADR-0004 §6 |
| REQ-903 | No third-party trademark, company or artist name in product paths (`tools/guard/check_repo.py` passes) | M | R | AGENTS rule 2 |
| REQ-904 | Every bundled asset has a row in `assets/LICENSES.md` (source, licence, SHA-256). Accepted licences per redistributable-assets §3.6: CC0, CC BY 4.0, MIT, BSD, GPL-2.0-or-later, GPL-3.0, AGPL-3.0, or project-generated. NC, ND, "no redistribution" and "no profit" terms are rejected | M | R | assets §1.5–1.6, §3.6 |
| REQ-905 | Every dependency is on the ADR-0006 approved list and in `THIRD_PARTY.md`. No ASIO SDK or other non-redistributable SDK is committed | M | R | ADR-0006 D2 |
| REQ-906 | No GPLv2-only code (e.g. rakarrack-plus) is copied. Copied files keep their original headers | M | R | pa §1.2, §1.8 |
| REQ-907 | The build follows ADR-0006: presets `windows-msvc`/`release`, `/fp:precise`, static CRT; CI produces Standalone + VST3 artefacts | M | I | ADR-0006 |

## 10. Open questions from the research notes

| Question | Resolution |
|---|---|
| ref Q1 modulation position | Switchable pre-amp / post-cab (REQ-415) |
| ref Q2 Transpose/Doubler latency | Doubler v1 with zero latency (REQ-418). Transpose post-v1 (REQ-420). Constant-latency rule (REQ-303) |
| ref Q3 amp blend, Q4 DI blend | Post-v1 (REQ-420) |
| ref Q5 channels per preset | All 3 per preset; switching is a parameter (REQ-406) |
| ref Q6 Global Lock storage | Lock on/off in the settings file (REQ-504); the locked blocks stay in the preset and are skipped on load (REQ-503, ADR-0003 §7) |
| os Q1 global vs per-block HQ | Global; offline forces HQ (REQ-421). Factors: #27 |
| os Q3 / tt Q4 ASR gates | Owned by #27 (thresholds) and #29 (gate vs warn). Verified by REQ-422 |
| tt Q3 golden storage | Owned by #29 |
| ta Q1 harmonizer | Post-v1 (REQ-420) |
| nam Q3 slimmable quality | Tied to the global quality setting; details deferred to #26 |
| nam Q4, assets §1.3 default captures | Classic-DSP amps are the factory default (REQ-407). Own captures need a dedicated ADR (owner: #26) |
