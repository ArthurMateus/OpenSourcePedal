Closes #

## What and why
<!-- 2-5 sentences. Link the ADR/spec section this implements. -->

## Measurements / evidence
<!-- Tests added, numbers measured (FR, THD, aliasing, CPU %, latency, LUFS, null residual), sources cited. "n/a" for docs-only. -->

## Definition of done
- [ ] Acceptance criteria of the issue are met (each one ticked in the issue or listed here)
- [ ] Tests added/updated; `ctest` passes locally (or "n/a: no build yet")
- [ ] Real-time safe: no alloc / locks / syscalls / logging / exceptions on the audio thread; denormals handled
- [ ] Benchmarks within budget; no > 10% regression (or n/a)
- [ ] Golden renders unchanged, or updated with an explanation below
- [ ] Latency reported correctly (or n/a)
- [ ] New deps are AGPLv3-compatible and in `THIRD_PARTY.md`; new assets are in `assets/LICENSES.md`
- [ ] No third-party trademarks or artist names outside `docs/research/`
- [ ] ≤ ~400 changed lines (excluding generated files/goldens); conventional commits
- [ ] Docs/spec updated if behaviour changed

## Author
<!-- account/role (canvas name), e.g. acct1/dsp-engineer (Kestrel) -->

## Reviewer verdict
<!-- Reviewer (different account) posts a review comment ending with:
Verdict: APPROVE, reviewed-by: <account>/code-reviewer -->
