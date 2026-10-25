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
| Issue | Owner | PR | Reviewer | Status |
|---|---|---|---|---|
| #28 ADR-0006 build/deps | Moth acct3 | #34 | Lyra acct1 | MERGED (status Proposed in file) |
| #29 ADR-0007 test strategy | Corvid gemini | #35 | acct2 | MERGED |
| #25 ADR-0004 threading + RT rules | Moth | #37 | Corvid (queued) | in review |
| #21 requirements | Moth | | | in progress |
| #22 signal flow | Moth | | | queued |
| #26 ADR-0002 NAM runtime | Sable acct1 | | | in progress |
| #27 ADR-0005 oversampling (owns the ASR thresholds) | Sable | | | queued |
| #24 ADR-0003 preset schema | Lyra acct1 | | | in progress |
| #23 parameters | Lyra | | | queued |
| PR #38 guard exemption (assets/LICENSES.md, THIRD_PARTY.md) | acct2 | #38 | Lyra (queued) | in review |

Free agents: Wren (acct3) is idle after #7, a candidate for Phase 3 prep (walking skeleton) once ADR-0006 and ADR-0004 are merged.
Phase 2 gate: spec (requirements, signal flow, parameters, preset schema, threading, RT rules) + ADRs 0001–0007 merged.

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
