# Handoff: owner session (2026-10-27)

The owner's own Claude Code session (account `default`, role dsp-engineer and acting merge orchestrator) stopped on
2026-10-27 ~16:50Z because the owner switched Claude accounts. The new account takes over from here. Read `AGENTS.md`
first, then this note. The orchestrator roster in `origin/state:docs/state/ORCHESTRATOR.md` is older than this note:
trust this note on anything they disagree about.

## Owner decisions in force

- **Merge without a cross-account review (2026-10-27).** The owner approved squash-merging every open PR that is green
  in CI, bottom of each stack first, skipping the `Verdict: APPROVE` step of AGENTS.md §5. This still holds: keep
  merging green PRs. CI must be green on **every** job (Release `/WX`, Debug + RT checker, pluginval, guard,
  clang-format), not only the three the branch rule requires.
- **Repository auto-merge is enabled** (settings), so `gh pr merge --auto` works.
- **No UI yet.** Keep following the roadmap; the pedalboard UI stays in Phase 6.
- **Tone match (#117) is paused** until the owner records a DI. All four tone-match PRs are merged. When the owner asks,
  walk them through `docs/guides/recording-a-di.md` (AGENTS.md §9).
- **Don't ask the owner anything unless it is truly needed.** Pick the next work from the plan below and keep going. The
  new account has plenty of usage, so run independent work in parallel sessions (one writer per set of files).

## Merged on 2026-10-27 (22 PRs)

Modulation #128–#131 (#70 done), input/output/doubler #124–#126 (#74 done), tuner #122–#123 (#75 done), delay part 1
#116 and part 2 #132, cab tests #114 (#69 done), reverb part 1 #103, #105, #111 (#72 done), drive #102, #104 (#67
done), tone match #118–#121, DI guide #127. `main` was green after every merge.

## In flight: the merge queue

Merge these in this order. **#133 already has its main-sync commit pushed and its CI was running at the stop.**

| Order | PR | Stack | State at the stop |
|---|---|---|---|
| 1 | #133 delay voicings (BBD, tape) | delay, base `main` | synced with main, CI running |
| 2 | #134 delay block | on #133 | green before the sync, needs a sync after #133 |
| 3 | #61 plugin state round-trip (app) | base `main` | mergeable; only an additive `THIRD_PARTY.md` row conflicts |
| 4 | #135 spring tank | reverb part 2, base `main` | green |
| 5 | #136 shimmer | on #135 | open |
| 6 | #137 spring + shimmer modes in the Reverb block | on #136 | open |
| 7 | #139 full-band FIR stage-0 decimator | HQ drive (#112), base `main` | draft (WIP): finish, mark ready, then merge |
| 8 | #140 DriveRegion HQ on the FIR kind | on #139 | draft (WIP); see `docs/state/HANDOFF/112.md` on its branch |

Run the queue with `tools/merge-train/merge_train.py` (added with this note) from a **clean** checkout, in the
background:

```bash
python -u tools/merge-train/merge_train.py 133 134 61 135 136 137
```

For each PR, the script:
- retargets it to `main`;
- merges its parent's final head, then `origin/main`;
- resolves additive conflicts in the shared registries;
- runs clang-format;
- pushes, and waits up to 90 minutes for every CI job;
- squash-merges;
- retargets the children;
- deletes the branch.

It stops at the first conflict it can't resolve safely, and at the first failed check. Before running it on a new
stack, add the new child → parent pairs to `PARENT`.

### What went wrong today, and what the script now handles

1. **Deleting a stack parent's branch closes its children.** GitHub closed #129 when #128's branch was deleted (it
   was reopened). Always retarget the children to `main` *before* deleting the parent branch. Sometimes GitHub
   retargets on its own, and then the PATCH fails harmlessly.
2. **After a squash merge, a child conflicts with `main` on the parent's lines.** Merge the parent PR's final
   `headRefOid` into the child first, which gives shared history. If `main`'s tree then equals that head
   (`git diff --quiet <parent head> origin/main`), merge `main` with `-s ours`; that is exact, not a guess. Otherwise,
   merge `main` normally.
3. **Commit the sync merge even when the tree doesn't change.** Without `main` as an ancestor, GitHub keeps reporting
   conflicts.
4. **Shared registries conflict on every block PR:** `tests/bench/budget.json`, `tests/bench/osp_bench.cpp`,
   `dsp-core/CMakeLists.txt`, `tests/CMakeLists.txt`, `THIRD_PARTY.md`. Keep both sides, but git leaves the shared
   closing line (`})});`, `},`) outside the conflict. Repeat it for each side until the brackets balance, or the
   blocks nest. Then run clang-format, because the merged `#include` list ends up unsorted. #115 removes this problem.
5. **Semantic conflicts that git can't see.** #114 used `std::function`, but `main`'s `osp_bench.cpp` had dropped
   `<functional>`. Only the build catches these, so wait for CI before merging.
6. **CI can be slow** (33 minutes once) when several sessions push at the same time. GitHub can also report the
   pre-push head for a short while, and `gh` can hit TLS timeouts. The script retries.
7. **Auto-fix monitors and other sessions must not push to a branch the script is syncing.** Tell them to stand down
   before the script reaches their PR.

## Interrupted work

- **#112 HQ drive (FIR oversampler in DriveRegion):** part 1 (149 lines) was built locally; its session was told to
  push and write `docs/state/HANDOFF/112.md` on its branch. Read that note, then finish the bench entry (part 2).
- **#73 reverb part 2:** #135 → #136 → #137 are open. After #135 merges, merge `origin/main` into `dsp/73-shimmer`,
  then carry it down to `dsp/73-reverb-modes`. The session's note is `docs/state/HANDOFF/73.md` on `dsp/73-reverb-modes`.
- **Guitarist QA audit:** interrupted after it built `main` at `ac929c8`. It filed **#138** (one NaN parameter value
  poisons `DriveStage` permanently) before stopping; its partial harness sits outside the repo on the owner's machine
  (`C:\ospqa-harness`). **Run it again in full** after the queue is merged, as a qa-engineer agent in its own clone of a
  fixed `main` commit. Brief:
  - **every block:** input, gate, comp, drive voicings and DriveRegion, cab with each IR, EQ, chorus/flanger, phaser,
    vibe, tremolo, the mod block, delay voices and the delay block, reverb room/hall/plate/spring/shimmer, doubler,
    output limiter, tuner;
  - **function:** each effect does what it should when enabled vs bypassed;
  - **latency:** measured vs `getLatencySamples()`, plus the summed full-chain latency in ms at 48 kHz with 64/128/256
    buffers;
  - **levels:** RMS/peak in vs out, no unexpected jumps when switching on, the limiter at ≤ −0.3 dBTP;
  - **quality:**
    - silent-input noise floor and DC;
    - an exact bypass null;
    - THD+N on clean blocks;
    - drive aliasing;
    - clicks on parameter and mode changes;
    - denormals on tails;
    - NaN/Inf;
  - **real audio:** renders of `assets/di/strum-arp-01.wav` through each block and through clean, crunch and lead chains;
  - **sample rates:** 44.1/48/96 kHz with odd block sizes;
  - **output:** a report with a pass/fail table and a plain-language "guitarist summary". File issues only for
    reproduced defects.

## Next work (in this order; parallel where the files don't overlap)

1. **Finish the merge queue** above.
2. **Fix #138** (a NaN-safe clamp on the DriveStage setters, plus a fuzz test). It's small and can run alongside the
   queue.
3. **#115: split the shared registries per block.** Do it right after the queue empties, before new block PRs, because
   it touches every registry file.
4. **#95: the v1 signal graph** (architect + dsp-engineer):
   - wire input → gate → drive → amp → cab → EQ → mod → delay → reverb → doubler → output, as in
     `docs/spec/signal-flow.md`;
   - latency alignment;
   - the `gate.postAmp` tap;
   - the clean DI path.
   This is the milestone that makes the plugin playable. It is big, so split it into stacked PRs.
5. **Rerun the full QA audit** on `main` once #95 lands, so the whole chain is tested (latency, levels, quality).
6. **Smaller items:** #113 (CI runs `halfband_fir.py --check`), #83 part 2 (CI bench baselines), #89 (compressor THD
   probe), #47 (round-trip latency measurement), #94 (licence notices in Credits), #42 status check.
7. **Later:** Phase 5 amps, then Phase 6 presets and UI.

## Build environment on the owner's machine

- Use VS 2022 BuildTools: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.
  Don't rely on vswhere, which finds a VS 18 install with no VC tools.
- Use short clone paths (`C:\ospNN`) with `git config core.longpaths true`. Set `GIT_LFS_SKIP_SMUDGE=1` when LFS
  smudging fails (worktrees too).
- Edit files with an editor tool, not PowerShell `Set-Content`, which adds a BOM and mangles UTF-8. Use LF line endings.
- No artist names or trademarks in committed text (the guard enforces this).
