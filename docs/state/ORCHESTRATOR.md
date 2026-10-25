# Orchestrator state (LIVE, `state` branch)

> Last updated: 2026-10-25 by acct2/orchestrator (canvas name "Orchestrator", Maestro).
> To take over: paste `docs/MASTER_PROMPT.md` (from main) into a fresh agent with **RESUME** appended.

## Phase
- Phase 0 (Bootstrap): **DONE.** PR #2 merged; reviewed by Corvid (gemini): APPROVE. Projects board blocked (B-001).
- Phase 1 (Research): **IN PROGRESS** (issues #3–#13, milestone "Phase 1: Research").

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

## Waiting on
- Research PRs from all five agents.

## Next actions
1. As each research PR lands: route a cross-account review (Corvid preferred), then merge.
2. When #3, #6, #8 and #9 are merged, recruit the Architect (acct3, Opus) to start the Phase 2 ADR drafts early.
3. Heartbeat routine every 45 min.
