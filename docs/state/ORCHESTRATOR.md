# Orchestrator state

> Live state for whoever is orchestrating. To take over: paste `docs/MASTER_PROMPT.md` with **RESUME** appended.
> Last updated: 2026-10-25 by acct2/orchestrator (canvas: "Claude Code", Maestro).

## Phase
**Phase 0 (Bootstrap): in progress.** Next: Phase 1 (Research).

## Owner decisions taken at bootstrap (2026-10-25)
- License: **AGPL-3.0-or-later + JUCE 8** ([ADR-0001](../adr/0001-license-and-framework.md)).
- The orchestrator terminal runs on **acct2**, so keep acct2's worker load light.
- The Gemini-family agent uses the Maestri preset **"Antigravity"** (`agy --dangerously-skip-permissions`).
- Pacing: **parallel across all accounts** (3–5 workers at once), with handoffs when a limit is hit.

## Conventions decided by the orchestrator
- All agents push as GitHub user `ArthurMateus`, so GitHub "approve" is impossible (no self-approval). Branch protection on `main`
  requires a PR and the `guard` check (later also `build`/`test`), with 0 required approvals. The review is a PR comment with a
  `Verdict:` line from a different account. Recorded in AGENTS.md §5.
- Maestri presets: `Claude acct1`, `Claude acct2`, `Claude acct3` (all `--dangerously-skip-permissions`) and `Antigravity`.

## Roster
| Canvas name | Preset / account | Role | Ticket(s) | Status |
|---|---|---|---|---|
| Claude Code | default (= acct2) | orchestrator | #1 bootstrap | active |

## Waiting on
- (none)

## Next actions
1. Merge the bootstrap PR.
2. Recruit researchers (acct1 ×2, acct3 ×2, Antigravity ×1) and assign the Phase 1 research issues.
3. Create the heartbeat routine (every 45 min).
