# Orchestrator state (LIVE, `state` branch)

> **2026-10-27 ~19:45Z, owner session part 2 stopped (usage).** Read
> [`docs/state/HANDOFF/owner-session-2.md`](https://github.com/ArthurMateus/OpenSourcePedal/blob/state/docs/state/HANDOFF/owner-session-2.md)
> (on this branch) first; it is the newest state. In short: the merge queue is done (#133–#137, #61, #141 merged).
> Next: resume the train on #144 → #143 → #148 → #142, finish #112 (#139/#140), then #115, then #95 (ADR-0009 plan).

> **2026-10-27 ~16:50Z, owner-session handoff (account switch).** Read
> [`docs/state/HANDOFF/owner-session.md`](https://github.com/ArthurMateus/OpenSourcePedal/blob/main/docs/state/HANDOFF/owner-session.md)
> (PR #141) before this file; it is newer than the roster below. In short: the owner approved merging green PRs without
> a cross-account review. 22 PRs merged today. The queue left is #133 → #134 → #61 → #135 → #136 → #137 → #112's PR,
> run with `tools/merge-train/merge_train.py`. QA filed #138. Then #115, then #95.

> Last updated: 2026-10-27 ~00:55Z by acct2/orchestrator (canvas name "Orchestrator", Maestro).
> To take over: paste `docs/MASTER_PROMPT.md` (from main) into a fresh agent with **RESUME** appended.

## Phase
- Phase 0 (Bootstrap): **DONE** (PR #2). Projects board blocked (B-001).
- Phase 1 (Research): **GATE PASSED** 2026-10-25. 11 notes.
- Phase 2 (Spec & architecture): **GATE PASSED** 2026-10-25. ADR-0001..0007 Accepted; spec: requirements (REQ-1xx..9xx), signal-flow,
  parameters (226 v1 host params), preset-schema + JSON schema, threading, realtime-rules. Milestone closed.
- Phase 3 (Walking skeleton): IN PROGRESS. Phase 4 (DSP blocks): STARTED in parallel (it only needs the skeleton + RT checker, both merged).

## Owner decisions (2026-10-25)
- License AGPL-3.0-or-later + JUCE 8 (ADR-0001). The orchestrator runs on **acct2**, so keep acct2's worker load light.
- The Gemini-family agent uses the Maestri preset "Antigravity". Pacing: parallel across all accounts.

## Conventions
- All agents push as GitHub user ArthurMateus, so there is no GitHub approval. `main` protection: PR + `guard` check, 0 approvals, admins enforced.
  Reviews are PR comments with a `Verdict:` line from a different account.
- Each agent works in its own git worktree: `C:\projects\My stuff\OSP-worktrees\<name>`. Maestri launches it in `<wt>\.maestri\roles\<id>`, so it does `cd ../../..`.
- Maestri roles: OSP Researcher, OSP Reviewer, OSP Architect, OSP Engineer.
- New Claude terminals show two startup prompts: trust folder (Down, Enter) and external imports (Down, Enter).

## Roster
| Canvas | Preset / account | Role | Current | Queue | Reviews PRs from |
|---|---|---|---|---|---|
| Orchestrator | default = acct2 | orchestrator + fill-in reviewer | heartbeat | | acct1, acct3 when short (NOT owner-session PRs: same account) |
| Owner session | default = acct2 | dsp-engineer (owner's own Claude Code) | #74 (#124-#126), #75 (#122-#123), #117 tone match (#118-#121), #127 merged | | — |
| Corvid | Antigravity (gemini) | reviewer | IDLE (quota until ~2026-10-30 16:00) | | everyone |
| Lyra | Claude acct1 | dsp-engineer + reviewer | resync #114; review #124→#125→#126 | #71 part 2 (on #116) | acct3, owner |
| Sable | Claude acct1 | dsp-engineer + reviewer | resync #102 (→ Wren re-review), retarget #104 then merge; confirm #105; re-review #111 | review #118→#121 (tone match), #89 | acct3, owner |
| Wren | Claude acct3 | audio-io + devops + reviewer | resync #61 (THIRD_PARTY.md) + merge; re-review #102 | review #122→#123 (tuner); #115 (TOP); #83 pt2 | acct1, owner |
| Moth | Claude acct3 | dsp-engineer + reviewer | resync #103 + merge; resync #105; fix #111 | review #114, #116; #73 | acct1, owner |

2026-10-26 heartbeat: every worker had been idle since ~01:45Z (usage limit, no handoff; Moth had unpushed #72 work). All re-dispatched at 05:35Z.
2026-10-27 00:50Z heartbeat: all workers idle again since ~06:40Z (limits). Main moved (#108-#110 at ~06:33Z, #127 at 19:42Z), so #61, #102, #103, #105 and #114 are CONFLICTING on registry files (osp_bench.cpp, budget.json, CMakeLists, THIRD_PARTY.md). Resyncs sent to the authors; all re-dispatched. The owner session (acct2) opened 9 PRs (#118-#126), reviewed by acct1/acct3 only.

## ACCOUNT ALERT
- **Corvid (Antigravity/Gemini) quota exhausted** (2026-10-25 ~22:00), "resets in 114h", so about **2026-10-30 16:00**.
  The Gemini CLI preset no longer works (it redirects individuals to Antigravity). Corvid is back on the Antigravity
  preset, idle. Until then, all reviews are Claude-only (acct1 ↔ acct3, with acct2 filling gaps). Re-enable Gemini reviews after reset.

## Phase 3 (milestone 4)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #41 CMake skeleton | Wren acct3 | #53 | MERGED |
| #48 testsignals + CC0 assets | Corvid | #55 | MERGED |
| #45 RT checker | Sable acct1 | #59 | MERGED |
| #46 osp-render + osp-bench | Lyra acct1 | #77, #78 | MERGED |
| #76 clang-format config + CI check | Wren | #79 | MERGED (clang-format 19.1.5 is a required check) |
| #42 JUCE plugin target | Wren | #58 MERGED. #61 APPROVED acct2 @e8c41dd (range −60…+12, −60/−48/+12 round-trip) | Wren merges on green → closes #42 |
| #43 CI artifacts + RT-checker Debug job | Wren | #97 MERGED | DONE. Required-check change = owner action B-003 |
| #44 pluginval 9 | Wren | #106 MERGED (f4b4e29; the pluginval job passed at strictness 9) | DONE. Add the required check under B-003 |
| #47 latency detector + UI | unassigned | | |
| #83 CI r0 baselines + R gate | Wren | #110 part 1 (5-run median, template measure) APPROVED acct2 | part 2: CI r0 + gate on |

## Phase 4 (milestone 5)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #62 measurement harness | Sable | #80, #84 | MERGED (Signalsmith dsp FFT, ADR-0006 A1) |
| #63 oversampler + ADAA | Sable | #86, #96 MERGED. #109 part 3 FIR HQ + ADR-0005 A2 APPROVED acct2 (conditional: Status → Accepted) | Sable merges → closes #63 |
| #67 drive | Sable | #102 fix 6a9f552 (Adaa2::reset(x) prime + silence-after-reset test) | re-review: Wren. #104 APPROVED, stacked |
| #98 ADR-0005 A1 | Sable | #101 MERGED (9fe6dbc) | DONE |
| #99 HIIR release-asset mirror | Wren | #108 APPROVED acct2 (mirror SHA-256 verified independently) | Wren merges |
| #64 smoothing primitives | Moth acct3 | #82 | MERGED |
| #66 compressor | Lyra | #85, #87 MERGED (dea4eb3, approved by Wren) | DONE (issue closed) |
| #65 gate | Moth | #88 MERGED (c1d1a45) | DONE (issue closed) |
| #72 reverb (3 parts) | Moth | #103 APPROVED (Lyra); orchestrator resyncs 8942a0e, 18b65aa (conflict-only) → auto-merge on green. #105 (base main now) APPROVED conditional on a mix residual test. #111 plate (review: Sable) | |
| #73 shimmer + spring | Moth | | next, after the #92 review |
| #68 EQ | Lyra | #90, #91, #92 MERGED (8373ef2) | DONE (issue closed) |
| #69 cab IR (4 parts) | Lyra | #93, #100, #107 MERGED (eabb516). #114 part 4 retargeted to main, synced 212570b | review: Moth |
| #71 delay | Lyra | | starting (after the #103 re-review) |
| #94 third-party notices in Credits/installer (Signalsmith, HIIR, nlohmann, JUCE, VST3, ASIO) | — | | filed by Lyra; HIIR added |
| #95 engine v1 signal graph (Phase 5) | unassigned (architect) | | filed. +Moth: report round(sum D) not sum round(D), and add an end-to-end REQ-301 Live test |
| #89 compressor THD probe (82/110 Hz) | — (Lyra preferred) | | unblocked (#87 merged) |
| #70 mod | — | | queued |
| #74 input/output/doubler | owner session | #124 ← #125 ← #126 (stacked), green | review: Lyra |
| #75 tuner | owner session | #122 ← #123 (stacked), green | review: Wren |
| #117 tone match (ADR-0008) | owner session | #118 ← #119 ← #120 ← #121 (stacked on dsp/tonematch-base), green; #127 DI guide MERGED | review: Sable (formal Verdicts; advisory review done) |
| #71 delay | Lyra | #116 part 1 green | review: Moth |
| #115 split shared source/bench/budget registries per block | Wren, NOW TOP PRIORITY | | 5 PRs conflicting again on 2026-10-27 |
Spec amendment merged: #81 (REQ-TEST-003 = residual vs the per-sample smoothing-law reference, starting at the delivery block,
normalised to the larger settled RMS; parameters.md has no 16-sample coefficient updates).

## Pending orchestrator follow-ups
- ADR-0001 amendment merged (PR #30).
- Add a header rule to forbidden_names.txt: no generic words. Consider a names.d/ directory, to stop append conflicts.
- After every merge-from-main, run clang-format 19.1.5 (pip clang-format==19.1.5; exe in site-packages/clang_format/data/bin) before pushing. Use set -e scripts: a conflicted merge must abort the push (lesson from #86).
- forbidden_names.txt conflicts: resolve with a union merge (merge origin/main into the PR branch in a helper worktree, take `git show :2:F` + `:3:F`, dedupe with `awk '!seen[$0]++'`). To merge: poll check-runs for the head SHA until guard completes, then `gh pr merge --squash`.

- **No rebase + force-push on PR branches** (rule 4): sync by merge, so that approved SHAs stay valid (Sable rebased #101 on 2026-10-26; verified with `git range-diff`).
- **Stacked PRs:** retarget the child to `main` BEFORE `gh pr merge <parent> --delete-branch`. Deleting the base branch auto-CLOSES the child, and a closed PR cannot be retargeted (#91 on 2026-10-26). To recover: recreate the base ref at the parent's head SHA (`gh api -X POST repos/{owner}/{repo}/git/refs`), `gh pr reopen`, `gh pr edit --base main`, then delete the ref again.

## Waiting on
- Resyncs: Wren #61, Sable #102, Moth #103/#105, Lyra #114. Then the merges: #61, #103, #102 → #104, #105.
- Reviews: Lyra #124-#126, Wren #122-#123 + #102, Sable #118-#121 + #105/#111, Moth #114 + #116.

## Next actions
1. Verify that each resync is conflict-only (range-diff) and that the approvals hold; merge in stack order.
2. #115 lands ASAP, to stop the registry-conflict churn.
3. Unassigned: #47 latency, #70 mod, #89 (Sable after the tone-match reviews), #94 notices, #95 engine graph (Phase 5).
4. Rule 8 reminder to all: hand off before limits hit (all idle ~18h with no handoff on 2026-10-26).
5. Heartbeat routine every 45 min.
