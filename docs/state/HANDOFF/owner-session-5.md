# Handoff: owner session 5 - integration train and #95 QA DONE

Date: 2026-10-28 (~09:35Z train done, QA after). Claude orchestrator, default account (no CLAUDE_CONFIG_DIR).
Supersedes owner-session-4. Read AGENTS.md first.

## Completed

- Train `152 153 154 155 156 162 163 164` ran from clean `C:\osptrain` with the preserved runner
  `C:\osptrain-control\merge_train.py` + `test_registry_migration.py` (log `C:\osptrain-control\train.log`).
  Every PR merged only after all six required jobs SUCCESS. The migration resolver handled the
  `tests/CMakeLists.txt` explicit-list vs #115 glob conflicts on #153 and #164 (dry-run of #164 verified first).
  No manual conflict resolution, no force-push. All #154 mode tests, #155 tail-idle tests, #156 annotations/ctor carried.
- main is now `38c3b06`. #95, #158, #159, #160, #161 closed (auto-closed by PRs; verified on merged main).
- Final QA passes 2 and 3 on exact main, results posted on #95
  (https://github.com/ArthurMateus/OpenSourcePedal/issues/95#issuecomment-6469643995):
  Release CTest 280/280, Debug RT 281/281, pass2 7 fails all #145, pass3 0 fail + 2 known #145, fixes verified.
  QA handoff `docs/state/HANDOFF/95-qa.md` on `qa/95-post-merge-audit` (`d5ce476`) updated.
- Filed #165: `input_stage` bench R +36% since #155 with no input-stage change (bisected; likely bench ordering).
  `spring_tank` r0 is stale (over r0 before and after the train). CPU budget passes (full chain 4.4% / 25%).

## Clones

- `C:\osptrain`: train clone, idle. Branch heads deleted after merge; reset to origin/main before reuse.
- `C:\ospqa3`: `qa/95-post-merge-audit` pushed `d5ce476`; untracked `b.bat` and local `main` 544942c preserved.
  Private harness in `.git\qa-pass2` (cab check now shape-only), `.git\qa-pass3`.
- `C:\ospprev`: local-only QA clone (preflight integration + `qa/final-main` at 38c3b06), reports in `.git\*.md`, nothing pushed.
- `C:\osp115`, `C:\osp164review`: idle; their branches are merged/deleted.

## Phase 5 plan (started after QA, same session)

- Plan issue #166; PR #180 (`architect/166-phase5-amp-plan`, docs only): `docs/adr/0011-amp-models-v1.md` Proposed.
  Clone `C:\osp5plan` (sole writer of that branch).
- Slices opened: #167 research, #168 NAM deps + RtResampler, #169 NamAmpModel, #170 NAM rates, #171 preamp stage,
  #172 tone stack, #173 power amp, #174 ClassicAmpModel, #175 ModelAmpChannel (+ `amp.ch<N>.lowCutHz`),
  #176 load/hot-swap, #177 NAM calibration, #178 console/acoustic, #179 Phase 5 gate. File ownership per ADR-0011 D7.
- Start now in parallel: #167 and #168. #169/#174/#179 are blocked by #83 (bench API).
- #83 + #165 are assigned by the owner to ANOTHER account (bench/CI files only: `tests/bench/**`, `tools/bench/**`,
  `.github/workflows/build.yml`, ADR-0007 amendment or ADR-0012). Do not edit those files from Phase 5 work.

## Next (not started; owner decides)

- #165 bench ordering / r0 refresh (#83 CI baselines is related).
- Still on hold without owner authorization: #47, #94, #145, #146, #147. No custom UI.
- Roadmap: Phase 5 amps (engine amp slot is still AmpStub), then presets/UI, hardening, release.
- pluginval not installed locally; CI covers strictness 9.

## Phase 5 progress (session stopped at usage limit, 2026-10-28)

- Merged: #180 (ADR-0011, main 1724518, closes #166), #181 (research note, closes #167).
- Approved, auto-merge watcher was running (re-run `bash docs/state/HANDOFF/mergewatch.sh <PR>` if it died):
  #183 preamp (#171), #185 tone stack (#172). Merge only after all six CI jobs SUCCESS; a Debug failure in
  TunerTap is flaky #147, re-run that job.
- Still running as subagents when stopped (their clones C:\osp168, C:\osp173): #168 NAM deps + RtResampler,
  #173 power amp. Check for their PRs/handoffs; review like #183 (RT skim, measurements, file ownership), then merge.
- Decisions recorded: 2x-rate (88.2/96 Live) full-band aliasing judged at full-model level in #174 (comment on #174);
  sag patent: #173 must use generic RC supply model and note it.
- Follow-ups: docs PR reconciling parameters.md §1.4 vs ADR-0011 D3 coefficient-update wording; sympy in tools/requirements.txt.
- Next after stages: #174 (needs #171-#173 + #83), #169 (needs #168 + #83). #83/#165 are on another account.

## Update (resumed after the limit)

- Merged: #183 (#171 preamp), #185 (#172 tone stack), #184 (#173 part 1), #182 (#165), #186 (#83, the regression gate
  with per-processor `processorBaselines`; see docs/state/HANDOFF/83.md). main f9aa767.
- #187 = #173 part 2 (sag, default 0), merge watcher was running. Squash conflicts were resolved by keeping the
  branch's version after verifying main's files == #184 final head.
- #168: subagent resumed in C:\osp168 (part 1 pushed at 5cf97ed, PR to open; part 2 RtResampler next).
- #174: subagent started in C:\osp174. It now OWNS engine/include/osp/engine/AmpModel.h (interface only; #169 extends
  it). The decision is in the comments on #174/#169, and an ADR-0011 D7 wording fix is pending (do it in the docs PR
  with the parameters.md §1.4 reconciliation).
- New bench entries need CI-runner processorBaselines (amp.json, nam.json) calibrated from CI artifacts.

## Minimum product track (owner request 2026-10-28, evening)

Owner: "minimum product + the auto tones (from the youtube link), then I will test it". Owner lifted the
"no custom UI yet" hold for this minimal editor. Skipped for the MVP: #176 hot-swap, #177, #178 (other account may
still do it), #179 full gate, goldens/demos, sag, #47, #146, installer. NAM (#169/#170) continues but does not block.

Merged since the last note: #187 (sag, default 0), #188 (#168 p1), #190 (docs A1), #191 (#174 p1, classic amps, slot
delay = processing delay), #192 (bench gate warns on uncalibrated processors, ADR-0007 A2), #193 (#147), #194 (#145),
#195 (ADR-0011 A2).

Running subagents (one clone each, disjoint files):
- #175 classic-first ModelAmpChannel (C:\osp175)   -> then #200 factory presets
- #197 tone match fits amps + app API (C:\osp197)
- #198 minimal editor (C:\osp198)                   -> then #199 preset menu, then tone-match panel (needs #197)
- #169 NamAmpModel (C:\osp169), #168 part 2 RtResampler (C:\osp168), #174 part 2 amp bench (C:\osp174)
- Other account: #178 console/acoustic + guard notice allowlist (prompt given to owner).
Merge rule unchanged: all six CI jobs green. Watcher: docs/state/HANDOFF/mergewatch.sh <PR> <head-sha>.
Before the owner's tone-match test: walk them through docs/guides/recording-a-di.md (AGENTS §9).
