# Orchestrator state (LIVE, `state` branch)

> Last updated: 2026-10-25 by acct2/orchestrator (canvas name "Orchestrator", Maestro).
> To take over: paste `docs/MASTER_PROMPT.md` (from main) into a fresh agent with **RESUME** appended.

## Phase
- Phase 0 (Bootstrap): **DONE** (PR #2). Projects board blocked (B-001).
- Phase 1 (Research): **GATE PASSED 2026-10-25.** All 11 topics merged with dated sources (#14 #15 #16 #17 #18 #19 #20 #31 #32 #33 #36). Milestone closed.
- Phase 2 (Spec & architecture): **IN PROGRESS** (#21–#29).

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

## Phase 2 progress
| Issue | Owner | PR | Status |
|---|---|---|---|
| #28 ADR-0006 build/deps | Moth acct3 | #34 | MERGED |
| #29 ADR-0007 test strategy | Corvid | #35 | MERGED |
| #25 ADR-0004 threading + RT rules | Moth | #37 | MERGED (owns the 25 ms crossfade; worker = loader thread) |
| #21 requirements (76 REQ) | Moth | #40 | MERGED |
| #24 ADR-0003 preset schema | Lyra acct1 | #49 | MERGED |
| #26 ADR-0002 NAM runtime | Sable acct1 | #39 | MERGED (L_slot fixed latency, RT-safe resampler fork) |
| #27 ADR-0005 oversampling | Sable | #50 | CHANGES-REQ (IIR null impossible; adopt shared OS-1 drive domain) |
| #22 signal flow | Moth | #52 | CHANGES-REQ (align with ADR-0005; reword REQ-421) |
| #23 parameters | Lyra | — | in progress |

Decisions this round: shared OS-1 domain for Drive A+B at the hungriest factor; ADR-0005 owns the factors and filters; osp-render may force HQ, hosts follow the user setting.
Moth and Sable are connected over Maestri for direct coordination.

## Phase 3 (issues #41–#48)
| Issue | Owner | Status |
|---|---|---|
| #41 CMake skeleton | Wren acct3 | in progress |
| #48 testsignals + first assets | Corvid | in progress |
| #42 plugin target, #43 CI build, #44 pluginval, #45 RT checker, #46 render/bench, #47 latency | — | queued (after #41) |

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
