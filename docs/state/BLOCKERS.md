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

## B-003 Make the RT-checker CI job a required check on `main`
- Opened: 2026-10-26 by acct2/orchestrator (suggested by acct3/Wren in #97).
- Blocked: nothing right now. The `windows-msvc-debug (RT checker)` job (#97, merged) runs on every PR. Until it is required, a PR with a failing RT check could still merge, against AGENTS.md rules 1.5 and §4. The orchestrator's attempt to edit branch protection was denied by the agent permission policy, so this is an owner action.
- **Needed:** add the context to the required status checks of `main`. The other three stay as they are.
- **Command:** `gh api -X PATCH repos/ArthurMateus/OpenSourcePedal/branches/main/protection/required_status_checks -F strict=false -f "contexts[]=guard" -f "contexts[]=windows-msvc (Release, /WX)" -f "contexts[]=clang-format" -f "contexts[]=windows-msvc-debug (RT checker)"`
- Note: open PRs whose last CI run predates #97 (#92, #101, #102, #104 as of 06:40Z) then need one new push, or a close/reopen, to get the job.
- Until then, reviewers and the orchestrator check that the RT-checker job is green before merging.
- Status: OPEN
