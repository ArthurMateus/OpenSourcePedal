# CLAUDE.md

@AGENTS.md

The import above pulls in the whole constitution: rules, repo map, build/test commands, workflow, handoff protocol.
`AGENTS.md` is the source of truth. This file adds only Claude Code specifics.

## Claude Code specifics

- **Role prompts:** `.claude/agents/<role>.md`. If you were recruited with a role, follow that file.
  You may spawn these as subagents for fan-out work (research sweeps, test triage), but keep the total work small:
  every subagent draws on the same Pro account.
- **Slash commands** (`.claude/commands/`):
  - `/pickup-ticket <N>`: claim an issue and load its context and handoff note.
  - `/handoff <N>`: push WIP, write the handoff note, label the issue, notify the orchestrator, stop.
  - `/review-pr <N>`: run the review checklist and post a verdict comment.
  - `/audio-regress`: build, run golden renders and benchmarks, and summarize the deltas.
- **Accounts:** you run as `acct1`, `acct2`, `acct3` (from `CLAUDE_CONFIG_DIR`) or default. Use the account name in
  pickup comments and review verdicts (`reviewed-by: acct3/code-reviewer`). Find it with
  `echo $env:CLAUDE_CONFIG_DIR` (PowerShell) or `echo $CLAUDE_CONFIG_DIR` (bash).
- **Shell:** Windows. Both PowerShell and Git Bash are available. Quote paths, because the repo path contains a space
  (`C:\projects\My stuff\OpenSourcePedal`).
