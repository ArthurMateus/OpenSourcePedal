# Orchestrator state (LIVE, `state` branch)

> Last updated: 2026-10-26 ~06:00Z by acct2/orchestrator (canvas name "Orchestrator", Maestro).
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
| Orchestrator | default = acct2 | orchestrator + fill-in reviewer | heartbeat; reviewing #61 | | acct1, acct3 when short |
| Corvid | Antigravity (gemini) | reviewer | IDLE (quota until ~2026-10-30 16:00) | | everyone |
| Lyra | Claude acct1 | dsp-engineer | #69 part 2 Cab block; sync #93 | #89 | acct3 |
| Sable | Claude acct1 | dsp-engineer | #63 part 2 (HIIR Oversampler, new branch dsp/63-oversampler-hiir) | #67 drive | acct3 |
| Wren | Claude acct3 | audio-io + devops + reviewer | merge #58; re-review #87; retarget/fix #61; review #92 | #43, #44 | acct1 |
| Moth | Claude acct3 | dsp-engineer + reviewer | push WIP #72; fix #88; review #90, #91 | #72 reverb | acct1 |

2026-10-26 heartbeat: every worker had been idle since ~01:45Z (usage limit, no handoff; Moth had unpushed #72 work). All re-dispatched at 05:35Z.

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
| #42 JUCE plugin target | Wren | #58 MERGED (290b69f). #61 reopened + base main (ce28ca9) but the levelDb range fix (−60…+12) is still MISSING, and it is DIRTY again | Wren fixing, then acct2 re-reviews |
| #43 CI artifacts + RT-checker Debug job | Wren | #97 MERGED (2bf733a) | DONE. Required-check change = owner action B-003 (permission denied to the agent) |
| #44 pluginval 9 | Wren | | next after #61 |
| #47 latency detector + UI | unassigned | | |
| #83 CI r0 baselines + R gate | unassigned (release-devops) | | |

## Phase 4 (milestone 5)
| Issue | Owner | PR | Status |
|---|---|---|---|
| #62 measurement harness | Sable | #80, #84 | MERGED (Signalsmith dsp FFT, ADR-0006 A1) |
| #63 oversampler + ADAA | Sable | #86, #96 MERGED (ddfaf20). Issue reopened for part 3 (FIR HQ) | part 3: Sable, when free |
| #67 drive | Sable | #102 part 1 (base main) review: Wren. #104 part 2 APPROVED (Moth @a944a7e; stacked on #102) | merge #104 after #102 (retarget, then merge main; NO rebase) |
| #98 ADR-0005 A1 + D3 wording | Sable | #101 head 2d773f8 (rebased; range-diff = ca5d2dc + status flip), APPROVED acct2 | Sable merges on green |
| #99 HIIR release-asset mirror | release-devops (Wren after #43) | | filed |
| #64 smoothing primitives | Moth acct3 | #82 | MERGED |
| #66 compressor | Lyra | #85, #87 MERGED (dea4eb3, approved by Wren) | DONE (issue closed) |
| #65 gate | Moth | #88 MERGED (c1d1a45) | DONE (issue closed) |
| #72 reverb (3 parts) | Moth | #103 part 1 CHANGES-REQ (Lyra: unclamped setModulation → OOB, NaN decay sticks). #105 part 2 stacked (review: Sable) | Moth fixing #103, then part 3 plate |
| #68 EQ | Lyra | #90 MERGED (3bf87fc), #91 MERGED (11939e1, Moth re-approved), #92 base main | review: Wren |
| #69 cab IR (4 parts) | Lyra | #93, #100 MERGED. #107 part 3 IrLoader CHANGES-REQ (acct2: Debug job lacks lfs: true; LFS bandwidth cache suggested) | Lyra fixing, then part 4 |
| #71 delay | Lyra (after #69 part 3) | | queued |
| #94 third-party notices in Credits/installer (Signalsmith, HIIR, nlohmann, JUCE, VST3, ASIO) | — | | filed by Lyra; HIIR added |
| #95 engine v1 signal graph (Phase 5) | unassigned (architect) | | filed. +Moth: report round(sum D) not sum round(D), and add an end-to-end REQ-301 Live test |
| #89 compressor THD probe (82/110 Hz) | — (Lyra preferred) | | unblocked (#87 merged) |
| #70 mod, #71 delay, #73 shimmer/spring, #74 doubler/io, #75 tuner | — | | queued |
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
- Wren: #58 CI, then #61 fix, #92 review and #43. Lyra: #93 sync, #69 part 2. Moth: #91 re-review (+ merge in stack order), #93, #96, then #72. Sable: #67.

## Next actions
1. Merge #87, #88 and #90→#91→#92 (merge each in stack order, and retarget the next PR to main) once approved and green.
2. Re-review #61 after Wren's range fix. Then #42 closes.
3. Unassigned: #47 latency, #83 CI baselines, #89. Give them to whoever frees first (#83/#43/#44 to Wren).
4. Rule 8 reminder to all: hand off before limits hit (Moth left unpushed work on 2026-10-26).
5. Heartbeat routine every 45 min.
