---
description: Claim a GitHub issue and load its context and any handoff note
argument-hint: <issue-number>
---
Pick up issue #$ARGUMENTS following AGENTS.md §5 "Picking up a ticket":

1. Run `gh issue view $ARGUMENTS --comments`. Read the acceptance criteria and every comment.
2. Find your account (`$CLAUDE_CONFIG_DIR` basename, or `default`) and your role. Then run
   `gh issue comment $ARGUMENTS --body "Picked up by <account>/<role> (<canvas name>)"`.
3. If `docs/state/HANDOFF/$ARGUMENTS.md` exists on `origin/main` or on the issue's branch, read it and continue from "Exact next step".
   Remove the `handoff` label: `gh issue edit $ARGUMENTS --remove-label handoff`.
4. `git fetch origin`. If a branch for this issue exists (`git branch -r | grep "/$ARGUMENTS-"`), check it out. Otherwise create
   `<role>/$ARGUMENTS-<short-name>` from `origin/main`.
5. Summarise the plan in 3–6 bullets, then start working.
