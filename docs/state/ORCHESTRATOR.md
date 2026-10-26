# Orchestrator state (LIVE, `state` branch)

> Last updated: 2026-10-25 by acct2/orchestrator (canvas name "Orchestrator", Maestro).
> To take over: paste `docs/MASTER_PROMPT.md` (from main) into a fresh agent with **RESUME** appended.

## Phase
- Phase 0 (Bootstrap): **DONE** (PR #2). Projects board blocked (B-001).
- Phase 1 (Research): **GATE PASSED** 2026-10-25. 11 notes.
- Phase 2 (Spec & architecture): **GATE PASSED** 2026-10-25. ADR-0001..0007 Accepted; spec: requirements (REQ-1xx..9xx), signal-flow,
  parameters (226 v1 host params), preset-schema + JSON schema, threading, realtime-rules. Milestone closed.
- Phase 3 (Walking skeleton): IN PROGRESS. Phase 4 (DSP blocks): STARTED in parallel (it only needs the skeleton + RT checker, both merged).

## Owner decisions (2026-10-25)
- License AGPL-3.0-or-later + JUCE 8 (ADR-0001). The orchestrator runs on **acct2**, so keep acct2's worker load light.
- The Gemini-family agent uses the Maestri preset "Antigravity". Pacing: parallel across all accounts.

## Conventions
- All agents push as GitHub user ArthurMateus, so there is no GitHub approval. `main` protection: PR + `guard` check, 0 approvals, admins enforced.
  Reviews are PR comments with a `Verdict:` line from a different account.
- Each agent works in its own git worktree: `C:\projects\My stuff\OSP-worktrees\<name>`. Maestri launches it in `<wt>\.maestri\roles\<id>`, so it does `cd ../../..`.
- Maestri roles: OSP Researcher, OSP Reviewer, OSP Architect, OSP Engineer.
- New Claude terminals show two startup prompts: trust folder (Down, Enter) and external imports (Down, Enter).

## Roster
| Canvas | Preset / account | Role | Current | Queue | Reviews PRs from |
|---|---|---|---|---|---|
| Orchestrator | default = acct2 | orchestrator | Phase 1 coordination | | |
| Corvid | Antigravity (gemini) | reviewer + researcher | #13 test-tooling | #11, #12 | everyone |
| Lyra | Claude acct1 | researcher | #3 reference-plugins | #4 tone-targets-a | acct3 |
| Sable | Claude acct1 | researcher | #6 nam-runtime | #10 oversampling | acct3 |
| Wren | Claude acct3 | researcher | #5 tone-targets-b | #7 redistributable-assets | acct1 |
| Moth | Claude acct3 | researcher | #8 asio-low-latency | #9 framework-comparison | acct1 |

Unassigned Phase 1: #11 time-based-effects and #12 prior-art go to whoever frees first.

## Phase 3 (milestone 4)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #41 CMake skeleton | Wren acct3 | #53 | MERGED (build job required on main) |
| #48 testsignals + CC0 assets (lossy interim) | Corvid | #55 | MERGED |
| #45 RT checker | Sable acct1 | #59 | MERGED |
| #42 JUCE plugin target | Wren | #58 | review: Corvid |
| #46 osp-render + osp-bench | Lyra acct1 | | in progress |
| #76 clang-format config + CI check | Wren | | next |
| #43 CI artifacts, #44 pluginval | Wren | | after #58 |
| #47 latency detector + UI | (unassigned) | | Corvid after the #58 review |
Phase 3 gate: ASIO standalone with measured latency (the owner can verify on hardware later; CI covers the offline detector), VST3 passes pluginval, CI artifacts.

## Phase 4 (milestone 5): issues #62–#75
| Issue | Owner | Status |
|---|---|---|
| #62 measurement harness + FFT choice | Sable | in progress (blocks the others' measurements) |
| #63 Oversampler + ADAA | Sable | next |
| #64 smoothing primitives | Moth acct3 | in progress |
| #65 gate | Moth | next |
| #66 comp, #67 drive, #68 EQ, #69 cab, #70 mod, #71 delay, #72/#73 reverb, #74 doubler/io, #75 tuner | — | queued |

Review routing: acct1 (Lyra, Sable) ↔ acct3 (Wren, Moth) ↔ gemini (Corvid); acct2 (orchestrator) fills gaps.
Usage notes: acct3 hit its limit at about 17:35, reset at 21:20. Agents are told to /clear before new tickets.

## Pending orchestrator follow-ups
- ADR-0001 amendment merged (PR #30).
- Add a header rule to forbidden_names.txt: no generic words. Consider a names.d/ directory, to stop append conflicts.
- forbidden_names.txt conflicts: resolve with a union merge (merge origin/main into the PR branch in a helper worktree, take `git show :2:F` + `:3:F`, dedupe with `awk '!seen[$0]++'`). To merge: poll check-runs for the head SHA until guard completes, then `gh pr merge --squash`.

## Waiting on
- Research PRs listed above.

## Next actions
1. As each research PR lands: route a cross-account review (Corvid preferred), then merge.
2. When #3, #6, #8 and #9 are merged, recruit the Architect (acct3, Opus) to start the Phase 2 ADR drafts early.
3. Heartbeat routine every 45 min.
