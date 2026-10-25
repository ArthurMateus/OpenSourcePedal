# Blockers needing the owner

Only problems agents cannot solve (gh auth, admin installs, locked accounts, paid licences).
Format: `## B-NNN <title>` · opened date · who is blocked · **what is needed** · **exact command for the owner** · status.

## B-001 GitHub token lacks the `project` scope (Projects board)
- Opened: 2026-10-25 by acct2/orchestrator
- Blocked: creating the GitHub Projects board (everything else proceeds; milestones and labels are used for tracking meanwhile).
- **Needed:** the owner refreshes the gh token with project scopes.
- **Command:** `gh auth refresh -s project,read:project`
- Status: OPEN
