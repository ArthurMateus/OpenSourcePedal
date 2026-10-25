---
description: Build, run unit/golden/RT-safety tests and benchmarks, and summarise deltas vs main
---
Run the audio regression suite (AGENTS.md §3–§4) on the current checkout:

1. `cmake --preset windows-msvc` (if the build dir is missing) and `cmake --build --preset release`.
2. `ctest --preset release --output-on-failure`. Record the pass/fail counts and the names of failing tests.
3. Golden renders: `ctest --preset release -L golden --output-on-failure`. For each preset, list the null residual (dBFS)
   and the spectral distance against the tolerance.
4. Benchmarks: run `build/release/tests/osp-bench` and compare with `tests/bench/budget.json` (flag any regression above 10% and any budget breach).
5. If the VST3 changed: `pluginval --strictness-level 8 --validate <path-to-.vst3>`.
6. Print a compact Markdown table: suite | result | notable deltas. If a PR number was given in the conversation, post it
   with `gh pr comment <N> --body-file <file>`.

If a step cannot run yet (for example, before Phase 3 there is no build system), say so explicitly. Never report it as passed.
