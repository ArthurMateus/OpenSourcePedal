# Roadmap

Each phase is a GitHub milestone. The orchestrator decides whether a gate passes against these criteria and records the decision in
[`state/ORCHESTRATOR.md`](state/ORCHESTRATOR.md).

| # | Phase | Gate criteria |
|---|---|---|
| 0 | Bootstrap | Constitution, roles, commands, docs tree, GitHub labels/milestones/templates/board, branch protection, canvas roster. |
| 1 | Research | Every research topic below is written up in `docs/research/` with dated sources. |
| 2 | Spec & architecture | `docs/spec/*` complete; ADRs for license, framework, NAM runtime, preset schema, threading, oversampling, deps, tests. |
| 3 | Walking skeleton | Standalone passes audio through ASIO with measured latency; VST3 loads in a host and passes pluginval ≥ 8; CI builds artifacts. |
| 4 | DSP blocks | Every block in the chain implemented and tested (§4 of AGENTS.md). |
| 5 | Amps | NAM loader + default classic-DSP amp models, cab IR loader (2 IRs + blend), full chain within CPU budget. |
| 6 | Presets & UI | Complete UI; factory presets (several per inspiration); user presets with import/export and migration. |
| 7 | Hardening | Soak tests, device hot-plug, sample-rate changes, crash-free sessions, docs complete. |
| 8 | v1.0 release | Installer, GitHub Release, changelog, demo clips. |
| 9 | Post-1.0 backlog | More blocks, CLAP, more presets, Linux/macOS builds… until nothing worthwhile is left; final status report. |

## Phase 1 research topics

| Topic | Note |
|---|---|
| Commercial artist-signature amp-sim lineup: signal chains, block types, controls | `research/reference-plugins.md` |
| Tone targets per inspiration, part A (technical terms) | `research/tone-targets-a.md` |
| Tone targets per inspiration, part B | `research/tone-targets-b.md` |
| NAM / RTNeural integration and CPU cost | `research/nam-runtime.md` |
| Redistributable `.nam` captures, IRs and DI test clips | `research/redistributable-assets.md` |
| ASIO SDK licensing, low-latency ASIO and WASAPI-exclusive practice | `research/asio-low-latency.md` |
| JUCE 8 vs iPlug2 (and others) | `research/framework-comparison.md` |
| Oversampling and anti-aliasing for nonlinear blocks | `research/oversampling.md` |
| Reverb, shimmer, delay and modulation algorithms | `research/time-based-effects.md` |
| Open-source prior art (licence-compatible) | `research/prior-art.md` |
| Measurement and test tooling (LUFS, null tests, RT checkers, pluginval, benchmarks) | `research/test-tooling.md` |
