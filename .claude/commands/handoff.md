---
description: Push WIP, write a handoff note, label the issue, notify the orchestrator, then stop
argument-hint: <issue-number>
---
Hand off issue #$ARGUMENTS following AGENTS.md §5 "Handing off":

1. `git add -A && git commit -m "wip: handoff #$ARGUMENTS"` (skip this if there is nothing to commit), then `git push -u origin HEAD`.
2. Copy `docs/state/HANDOFF/_TEMPLATE.md` to `docs/state/HANDOFF/$ARGUMENTS.md` and fill in every section honestly:
   what is done, what is left, the exact next step (a command or file:line), failing tests with their output, and gotchas.
   Commit it (`docs: handoff note for #$ARGUMENTS`) and push.
3. Run `gh issue comment $ARGUMENTS --body "Handoff: see docs/state/HANDOFF/$ARGUMENTS.md on branch <branch>. Next step: <one line>"`
   and `gh issue edit $ARGUMENTS --add-label handoff`.
4. Run `maestri list` to get the orchestrator's name, then `maestri ask "<orchestrator>" "HANDOFF #$ARGUMENTS on <branch>: <one line>"`.
5. Stop. Do not start other work.
