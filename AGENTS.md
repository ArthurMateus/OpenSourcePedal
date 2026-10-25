# AGENTS.md: OpenSourcePedal constitution

> **Source of truth.** `CLAUDE.md` and `GEMINI.md` mirror this file. If you change a rule, change it here first,
> then update the mirrors in the same PR.

OpenSourcePedal is an open-source guitar amp-and-effects processor for Windows (standalone app + VST3), *inspired
by* modern artist-signature amp-sim plugins. It is built by a team of autonomous agents coordinated by an
orchestrator on the Maestri canvas. The full mission brief is in [`docs/MASTER_PROMPT.md`](docs/MASTER_PROMPT.md).

## 1. Rules (non-negotiable)

1. **Never ask the owner questions.** Decide, record it (an ADR in `docs/adr/` or a comment on the issue), and move on.
   If you are truly blocked by something only the owner can fix, follow §7 "Blockers".
2. **"Inspired by" only.** No third-party trademarks, company names (including "Neural DSP" and "Archetype") or artist names
   in the product, UI, presets, shipped assets, release notes or marketing copy. Factory presets get original names.
   Internal research notes (`docs/research/`) may say "aims at a tone similar to X". Attribution files
   (`assets/LICENSES.md`, `THIRD_PARTY.md`, the in-app Credits text) may name upstream sources and original asset titles
   only where a licence requires or provenance needs it.
   **Never** disassemble, decompile or extract anything from a commercial plugin. Every bundled asset (`.nam`
   capture, IR, DI clip, font, image) has an entry in `assets/LICENSES.md` with its source and license.
3. **Never commit** secrets, SDKs we may not redistribute, large binaries outside Git LFS (> 1 MB → LFS), or any file
   whose license you cannot state.
4. **Never** force-push to `main`, rewrite published history, or delete the repo, releases, or branches someone else
   is working on.
5. **Real-time rules are not optional** (§4). A performance or real-time regression blocks the merge.
6. **Back technical claims** with a source (URL + access date), a measurement, or a test. Write "unverified" when you
   have none of these.
7. **Small PRs:** aim for about 400 changed lines or fewer, excluding generated files and golden renders.
8. **Leave state in files before you stop**, for any reason: push the branch, write the handoff note, comment on the issue.
9. The project license is **AGPL-3.0-or-later** ([ADR-0001](docs/adr/0001-license-and-framework.md)). Any third-party code
   you add must be AGPLv3-compatible (MIT, BSD, ISC, Apache-2.0, zlib, LGPL, GPLv3, AGPLv3) and must be listed in
   `THIRD_PARTY.md`. GPLv2-only code is **not** compatible.

## 2. Repository map

```
dsp-core/      Pure C++20 DSP blocks. NO JUCE, NO OS headers. Portable to embedded later.
engine/        Signal graph, parameter model, preset load/save/migrate, NAM/IR loading. No UI. No JUCE in public headers.
app/           JUCE AudioProcessor + standalone + VST3 wrappers, device handling, latency measurement.
ui/            JUCE editor components (pedalboard, amp page, preset browser, tuner, meters). Original vector art only.
tests/         Unit tests (Catch2), golden-render regression, RT-safety and fuzz tests, benchmarks.
tools/         Offline render CLI (osp-render), analysis scripts (spectrum, LUFS, null test), preset linters.
presets/       Factory presets (versioned JSON).
assets/        Bundled IRs / captures / DI clips (Git LFS) + assets/LICENSES.md.
cmake/         CMake helpers, dependency fetching (JUCE, NAM core, Eigen, ...).
docs/adr/      Architecture Decision Records (NNNN-title.md).
docs/research/ Research notes with sources and dates.
docs/spec/     Requirements, signal flow, parameters, preset schema, threading, RT rules.
docs/state/    BLOCKERS.md, HANDOFF/<issue>.md, ORCHESTRATOR.md (a pointer: the LIVE copy is on the `state` branch, see §5).
docs/demos/    Rendered demo clips per milestone.
```

## 3. Build and test (from Phase 3 on. Commands are final once `CMakePresets.json` lands)

Requirements: Visual Studio 2022 (MSVC v143, C++20), CMake ≥ 3.25, Ninja (optional), Git LFS, Python 3.11+ for tools.

```powershell
cmake --preset windows-msvc              # configure (fetches JUCE at configure time; ASIO headers come with JUCE)
cmake --build --preset release           # build standalone, VST3, tests, tools
ctest --preset release                   # unit + golden + RT-safety tests
./build/release/tools/osp-render --preset presets/factory/<name>.json --in assets/di/<clip>.wav --out out.wav
./build/release/tests/osp-bench          # CPU benchmarks, compared against tests/bench/budget.json
pluginval --strictness-level 8 --validate build/release/app/OpenSourcePedal_artefacts/Release/VST3/OpenSourcePedal.vst3
```

## 4. Quality gates (agents cannot listen, so they measure)

Every DSP/audio change passes, in CI:
- **Unit tests per block:** frequency response, THD and aliasing (sweeps), gain staging, parameter smoothing (no
  zipper noise), bypass nulls against input, all of 44.1/48/88.2/96 kHz and block sizes 32…2048 (including odd sizes).
- **Golden renders:** fixed DI clips through every factory preset, compared with stored renders (null test + spectral
  distance tolerances). An intended change updates the goldens in the same PR and explains why.
- **Real-time safety on the audio thread:** no allocation, locks, system calls, logging, exceptions or I/O.
  A debug-build checker enforces this. Flush denormals to zero. Fuzz parameters and assert no NaN/Inf.
- **Performance:** per-block and per-preset benchmarks. Budget: full chain at 48 kHz / 64 samples < 25% of one core on
  a mid-range CPU (CI runner as proxy, normalized). CI fails on > 10% regression.
- **Plugin validation:** `pluginval` strictness ≥ 8, plus a state save/load round-trip.
- **Latency:** processing latency (oversampling, linear-phase, lookahead) is reported to the host via `setLatencySamples`.

## 5. Workflow

- **One GitHub issue per unit of work**, with acceptance criteria. Labels: `phase:N-*`, `role:*`, `type:*`.
- **Branch** `<role>/<issue>-<short-name>` (e.g. `dsp/42-noise-gate`). One PR per issue. Conventional commits
  (`feat(dsp): …`, `fix(engine): …`, `docs: …`, `test: …`, `ci: …`, `chore: …`). PR body says `Closes #N`.
- **Merge** only when CI is green **and** a reviewer on a *different account* than the author has approved. All agents
  push as the same GitHub user, so GitHub's own "approve" button cannot be used. The reviewer posts a PR review
  comment ending with a verdict line:
  `Verdict: APPROVE, reviewed-by: <account>/<role>` or `Verdict: CHANGES-REQUESTED, reviewed-by: <account>/<role>`.
  The orchestrator (or the author, once approved) squash-merges with `gh pr merge --squash --delete-branch`.

### Picking up a ticket
1. `gh issue view <N>`. Read the acceptance criteria and every comment.
2. Comment on the issue: `Picked up by <account>/<role> (<agent name on canvas>)`.
3. If `docs/state/HANDOFF/<N>.md` exists, read it first and continue from "Exact next step".
4. `git fetch && git checkout <branch>` if the branch exists; otherwise branch from `origin/main`.

### Handing off (usage limit near, reassignment, or stopping for any reason)
1. Commit WIP (`wip: …` is fine on a feature branch) and `git push -u origin <branch>`.
2. Write `docs/state/HANDOFF/<N>.md` on the branch (template: `docs/state/HANDOFF/_TEMPLATE.md`), commit and push.
3. `gh issue comment <N>` with a link to the note, then `gh issue edit <N> --add-label handoff`.
4. Tell the orchestrator over Maestri: `maestri ask "<orchestrator name>" "HANDOFF #<N>: <one line>"`. Then stop.

### Live state branch
`main` is protected (PRs only). The orchestrator's **live** state (`docs/state/ORCHESTRATOR.md`, `docs/state/BLOCKERS.md`)
is pushed directly to the unprotected branch **`state`**, so that heartbeat updates need no PR. Read it with
`git fetch origin state && git show origin/state:docs/state/ORCHESTRATOR.md`. Only the orchestrator writes to `state`.
Handoff notes stay on the feature branch of their issue.

### Talking to the team (Maestri)
- Run `maestri list` to see your teammates and notes. The orchestrator is the terminal you are connected to.
- When you finish a task you were given over Maestri, report back with `maestri ask "<orchestrator name>" "<result>"`.

## 6. Agent roles

The role prompts are in `.claude/agents/*.md` (Claude) and are usable as plain text by any agent:
orchestrator, researcher, architect, dsp-engineer, audio-io-engineer, ui-engineer, preset-designer, qa-engineer,
code-reviewer, release-devops, docs-writer.

## 7. Blockers

Only for things agents cannot solve (gh not authenticated, an install that needs admin rights, an account locked out,
a missing paid licence): add an entry to `docs/state/BLOCKERS.md` (what is needed + the exact command for the owner),
tell the orchestrator, and **keep working on everything that is not blocked**.

## 8. Usage limits

Pro accounts have usage limits. Before you run out, hand off (§5). Prefer cheaper models for mechanical
work (running tests, formatting, triage). Keep the strongest model for architecture, DSP design and review.
