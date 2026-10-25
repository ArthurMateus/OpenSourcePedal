---
description: Review a PR against the OpenSourcePedal checklist and post a verdict comment
argument-hint: <pr-number>
---
Review PR #$ARGUMENTS as a code reviewer (`.claude/agents/code-reviewer.md`).

1. `gh pr view $ARGUMENTS --json author,title,body,headRefName,files,commits,statusCheckRollup` and `gh pr diff $ARGUMENTS`.
   If the PR's pickup or commit trail shows it was authored on **your** account, stop and tell the orchestrator.
2. Check the linked issue's acceptance criteria (`Closes #N`) with `gh issue view N`.
3. Go through the checklist. For each item, cite file:line for any problem:
   - [ ] Real-time safety: no alloc, locks, syscalls, logging, exceptions or I/O on the audio thread; denormals handled; smoothing.
   - [ ] Correctness: math, sample-rate dependence, edge-case block sizes, reset, latency reporting, thread safety.
   - [ ] Tests cover the acceptance criteria; measurements are in the PR body; golden changes are explained.
   - [ ] CI is green (`statusCheckRollup`).
   - [ ] Licensing: deps are AGPLv3-compatible and in THIRD_PARTY.md; assets are in assets/LICENSES.md; no SDK or secrets committed.
   - [ ] "Inspired by": no artist, company or product names outside docs/research/.
   - [ ] Scope: about 400 lines or fewer, conventional commits, `Closes #N`.
4. If you can, build and run the relevant tests locally (`/audio-regress` for DSP changes).
5. Write the review to a temp file and post it with `gh pr review $ARGUMENTS --comment --body-file <file>`.
   Separate blocking issues from nits. The last line must be exactly:
   `Verdict: APPROVE, reviewed-by: <account>/code-reviewer` or `Verdict: CHANGES-REQUESTED, reviewed-by: <account>/code-reviewer`.
6. Report the verdict to the orchestrator over Maestri.
