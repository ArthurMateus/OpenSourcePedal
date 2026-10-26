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

## ACCOUNT ALERT
- **Corvid (Antigravity/Gemini) quota exhausted** (2026-10-25 ~22:00), "resets in 114h", so about **2026-10-30 16:00**.
  The Gemini CLI preset no longer works (it redirects individuals to Antigravity). Corvid is back on the Antigravity
  preset, idle. Until then, all reviews are Claude-only (acct1 ↔ acct3, with acct2 filling gaps). Re-enable Gemini reviews after reset.

## Phase 3 (milestone 4)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #41 CMake skeleton | Wren acct3 | #53 | MERGED |
| #48 testsignals + CC0 assets | Corvid | #55 | MERGED |
| #45 RT checker | Sable acct1 | #59 | MERGED |
| #46 osp-render + osp-bench | Lyra acct1 | #77, #78 | MERGED |
| #76 clang-format config + CI check | Wren | #79 | MERGED (clang-format 19.1.5 is a required check) |
| #42 JUCE plugin target | Wren | #58 (APPROVED, needs a main merge), #61 (state blob; review acct2; fix the output.levelDb range) | in progress |
| #43 CI artifacts, #44 pluginval 9 | Wren | | after #58/#61 |
| #47 latency detector + UI | unassigned | | |
| #83 CI r0 baselines + R gate | unassigned (release-devops) | | |

## Phase 4 (milestone 5)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #62 measurement harness | Sable | #80, #84 | MERGED (Signalsmith dsp FFT, ADR-0006 A1) |
| #63 oversampler + ADAA | Sable | #86 part 1 MERGED (ADAA) | part 2 (HIIR Oversampler) in progress |
| #64 smoothing primitives | Moth acct3 | #82 | MERGED |
| #66 compressor | Lyra | #85 MERGED, #87 CHANGES-REQ | fixing |
| #65 gate | Moth | #88 | review: Lyra |
| #72 reverb part 1 | Moth | | in progress |
| #68 EQ | Lyra | | next |
| #67 drive (needs #63 part 2), #69 cab, #70 mod, #71 delay, #73 shimmer/spring, #74 doubler/io, #75 tuner | — | | queued |
Spec amendment merged: #81 (REQ-TEST-003 = residual vs the per-sample smoothing-law reference, starting at the delivery block,
normalised to the larger settled RMS; parameters.md has no 16-sample coefficient updates).

## Pending orchestrator follow-ups
- ADR-0001 amendment merged (PR #30).
- Add a header rule to forbidden_names.txt: no generic words. Consider a names.d/ directory, to stop append conflicts.
- After every merge-from-main, run clang-format 19.1.5 (pip clang-format==19.1.5; exe in site-packages/clang_format/data/bin) before pushing. Use set -e scripts: a conflicted merge must abort the push (lesson from #86).
- forbidden_names.txt conflicts: resolve with a union merge (merge origin/main into the PR branch in a helper worktree, take `git show :2:F` + `:3:F`, dedupe with `awk '!seen[$0]++'`). To merge: poll check-runs for the head SHA until guard completes, then `gh pr merge --squash`.

## Waiting on
- Research PRs listed above.

## Next actions
1. As each research PR lands: route a cross-account review (Corvid preferred), then merge.
2. When #3, #6, #8 and #9 are merged, recruit the Architect (acct3, Opus) to start the Phase 2 ADR drafts early.
3. Heartbeat routine every 45 min.
