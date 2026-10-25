# Blockers needing the owner

Only problems agents cannot solve (gh auth, admin installs, locked accounts, paid licences).
Format: `## B-NNN <title>` · opened date · who is blocked · **what is needed** · **exact command for the owner** · status.

## B-001 GitHub token lacks the `project` scope (Projects board)
- Opened: 2026-10-25 by acct2/orchestrator
- Blocked: creating the GitHub Projects board (everything else proceeds; milestones and labels are used for tracking meanwhile).
- **Needed:** the owner refreshes the gh token with project scopes.
- **Command:** `gh auth refresh -s project,read:project`
- Status: OPEN

## B-002 Original-quality CC0 cab IRs / DI clip from Freesound (non-urgent)
- Opened: 2026-10-25 by acct2/orchestrator (found by acct3/Wren reviewing PR #55).
- Blocked: replacing the interim, preview-derived (lossy 110 kb/s Vorbis) CC0 IRs and DI clip with the original WAV files. Downloading originals from Freesound requires a logged-in account. Everything else proceeds: the interim files are CC0 and usable for development, and designed IRs are also planned.
- **Needed:** a free Freesound account and an API key/OAuth token that agents may use for downloads.
- **Command:** create an account at https://freesound.org/home/register/, then an API credential at https://freesound.org/apiv2/apply/, and store the token as a user env var: `setx FREESOUND_TOKEN "<token>"`. Agents will read it from the environment and never commit it.
- Status: OPEN (optional; the project can ship with designed IRs instead)
