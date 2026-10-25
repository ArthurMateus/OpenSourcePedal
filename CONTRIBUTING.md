# Contributing

Everyone, human or agent, follows [`AGENTS.md`](AGENTS.md). The short version:

1. Work from a GitHub issue that has acceptance criteria. Comment on the issue to claim it.
2. Branch as `<role>/<issue>-<short-name>` from `main`, and keep PRs to about 400 changed lines or fewer.
3. Use conventional commits (`feat(dsp): …`, `fix(app): …`, `docs: …`, `test: …`, `ci: …`), and put `Closes #N` in the PR body.
4. CI must be green: unit tests, golden renders, the RT-safety checker, benchmarks against their budget, and pluginval ≥ 8.
5. A reviewer on a different account posts a `Verdict: APPROVE` comment. Then the PR is squash-merged.

Hard rules:
- Real-time audio code never allocates, locks, logs, throws or makes system calls.
- No third-party trademarks or artist names in the product. No reverse engineering of commercial plugins.
- Every dependency is AGPLv3-compatible and listed in `THIRD_PARTY.md`. Every bundled asset is listed in `assets/LICENSES.md`.
- Never commit secrets or SDKs. Binaries over 1 MB go in Git LFS.
- Back technical claims with a source, a measurement or a test.

By contributing, you agree that your contribution is licensed under AGPL-3.0-or-later.
