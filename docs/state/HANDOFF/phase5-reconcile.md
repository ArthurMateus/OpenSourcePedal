# Handoff: Phase 5 docs/tools reconciliation

- From: ArthurMateus/docs-writer (Codex), 2026-10-28
- Branch: `docs/phase5-reconcile`
- Reason: implementation complete; six-job CI and merge checkpoint

## Done
- Read AGENTS.md, state-branch owner-session-5, #166, the #174/#169 coordination decisions and merged PRs #183/#185.
- Picked up the docs follow-up on #166. Fresh clone C:/ospdocs with LFS smudge skipped; other clones untouched.
- parameters.md section 1.4 and ADR-0011 D3 now distinguish expensive target/design work from the stable
  per-processing-sample coefficient trajectory. Parameter-domain smoothing with coefficient ramps and direct
  coefficient-domain glides are both allowed only with the unchanged REQ-TEST-003 <= -60 dBc double-reference gate.
- Cited merged #183's changed-target TPT coefficient glide and reported -93.6 dBc step residual; cited #185's
  adaptive chunk design and per-sample modal coefficient ramp, reported worst -90.8 dBc at 192 kHz.
  These are the PRs' reported measurements, not new measurements by this docs task. The failed 16-sample hold
  example from #87 remains to distinguish a held coefficient from an interpolated trajectory.
- Preserved original D7 table/ownership prose verbatim. Dated Amendment A1 records #174 creating AmpModel.h,
  #169 extending it, and #174's recursive engine source-discovery decision (source-list change only), while #168
  retains its separate PRIVATE NAM-core link. This records a decision, not a claim that #174 already merged.
- Added sympy==1.14.0 to tools/requirements.txt and corrected the derivation tool's stale dependency-help text.
- No DSP, engine, CMake, bench, third-party attribution or state-branch edits. No Phase 5 implementation work.

## Validation
- Isolated .git/tools-env: installed sympy==1.14.0 and numpy==2.2.6, then successfully ran the existing
  tools/amp/tonestack_derive.py (exit 0; all b0..b3 and a0..a3 coefficient groups emitted).
- SymPy installed metadata reports version 1.14.0, licence BSD; release source:
  https://pypi.org/project/sympy/1.14.0/ (accessed 2026-10-28). This pins the tool's existing import; no code vendored.
- python tools/guard/check_repo.py: 330 tracked files checked, zero problems before adding this note.
- git diff --check passed. A comparison with the base confirmed the original D7 section is preserved verbatim
  before the appended amendment. No audio algorithm changes; native Release/Debug RT validation runs in CI.

## Left to do
- Push, open one small PR referencing #166/#183/#185 without a closing keyword.
- Wait for ALL SIX checks on its head: windows-msvc Release /WX, windows-msvc-debug RT checker,
  pluginval v1.0.4 strictness 9, guard, clang-format, halfband-check. Owner waived cross-account review only.
- If the sole failure is the known TunerTap timing test #147, rerun that job and record it; do not edit unrelated code.
- Squash-merge after all six succeed; post results on #166. Final CI/merge verdict is recorded there and on the PR.

## Exact next step
`gh pr view docs/phase5-reconcile --json number,state,headRefOid,statusCheckRollup`.
If no PR exists, create it from the pushed branch. If MERGED, nothing remains. Otherwise wait for all six head
checks, then `gh pr merge <N> --squash --delete-branch`.

## Failing tests / known breakage
No local failure. Only dependency/help text and documentation changed; LFS assets were not needed locally.

## Gotchas
- Work only in C:/ospdocs. Helpers, tool environment and derivation output are inside .git.
- Other agents own engine, dependency/build files, amp sources/tests/bench entries and handoffs 168/173/174.
- Preserve published history; merge origin/main to update. Do not write state or start other tickets.
