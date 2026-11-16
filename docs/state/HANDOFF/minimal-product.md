# Minimal product: status and how to test it (2026-11-16)

The owner asked for a minimal product: some effects plus the main feature, matching the guitar tone of a song
against the owner's own DI (#117, ADR-0008). The scaling work comes after this works. It is all on `main`.

## What exists

- **Standalone app and VST3** with the v1 graph: input, gate, compressor, drive, amp (three classic channels
  `clean_a`, `crunch_a`, `lead_a`), cab, EQ, modulation, delay, reverb, doubler, output, tuner.
- **Editor** (`ui/`): block row, block pages, amp page, meters, tuner, latency readout, a preset bar (load, save as a user
  preset) and the tone-match panel (song or link, DI, part to match, Match, Cancel, Audition, Save preset).
- **Factory presets** in `presets/factory/` (five categories, original names, level-matched).
- **Tone match** (`engine/`, `tools/osp-tonematch`): fits the classic amp, gain, tone controls, cab and EQ to a reference.
  App API with progress and cancel; ADR-0008 amendment A1 has the limits.
- **Portable package:** `tools/package/package.ps1` builds `dist/OpenSourcePedal-win64.zip`. The app finds
  `assets/` and `presets/factory/` next to the exe. User guide: `docs/guides/quickstart.md`.

## Verified (2026-11-10, commit bc8692c)

- CI green on `main` (all six jobs); clean clone, Release build and `ctest`: 384/384 pass (with the Git LFS files).
- The standalone app starts, the window and preset menu show the five factory categories, from the packaged folder too.
- `osp-tonematch` on a stand-in reference (the bundled DI through a distortion curve and low-pass): 6.7 s, residual 1.33 dB
  against 3.76 dB for the DI through a flat clean chain.

## Not verified

- A real song with the owner's own DI. The owner has not recorded one (walk them through
  `docs/guides/recording-a-di.md`; AGENTS.md §9). The bundled DI is dark and lossy, so clean matches sound muffled.
- Listening: nobody has heard the audio. Matching "exactly" is not achievable. Gain depends on playing style, and a
  different guitar gives a wrong gain.
- The Match button in the panel by hand, a YouTube link through the user's own `yt-dlp` and `ffmpeg`, and the audition
  on a real audio device.
- Other machines: only the owner's Windows 11 machine was used.

## Test it (a fresh agent or the owner)

1. Clone, then `git lfs pull` (without it 32 tests fail: the DI clip and IRs are pointer files).
2. From a VS 2022 developer shell: `cmake --preset windows-msvc`, `cmake --build --preset release`, `ctest --preset release`.
   VS 2022 BuildTools script: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.
3. `pwsh tools/package/package.ps1`, unzip, run `OpenSourcePedal.exe` (first start can take 20 s), then follow
   `docs/guides/quickstart.md`.
4. Tone match: `osp-tonematch --ref song.wav --di my-di.wav --out preset.json --render preview.wav --start 65 --end 100`.

## Next (in this order)

1. The owner records a DI; run a real song and fix what is wrong (gain, muffled clean tones, the wrong amp).
2. `osp-render --preset` still prints "not supported yet" although the preset loader exists (`tools/osp-render`).
3. Parked for later: NAM loading (#169, #170), amp hot-swap (#176), extra amp types (#177, #178, #179), golden renders and
   demos, the latency-measurement screen (#47), the installer, licence notices in Credits (#94).

## Merge script

`tools/merge-train/merge_train.py` merges stacked PRs in order, waiting for all six CI jobs. Add new child to parent
pairs to `PARENT`. Run it in the background and keep the session alive: it dies with the shell that started it.
