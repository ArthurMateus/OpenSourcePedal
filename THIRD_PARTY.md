# Third-party software

Every dependency must be AGPLv3-compatible. Add a row in the same PR that introduces the dependency.

| Component | Version / pin | License | How obtained | Used in | Notes |
|---|---|---|---|---|---|
| Catch2 | v3.16.1 → `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | CMake FetchContent (`cmake/OspDependencies.cmake`) | `tests/` only (not shipped) | Pin checked against the v3.16.1 tag via the GitHub API on 2026-10-25 |
| nanobench | v4.6.0 → `b709728dba4df0356d467abfb0ba20781a8031c0` | MIT | CMake FetchContent (download only) | `tests/osp-bench` (not shipped) | Pin checked against the v4.6.0 tag via the GitHub API on 2026-10-25 |
| libebur128 | v1.2.6 → `67b33abe1558160ed76ada1322329b0e9e058b02` | MIT | CMake FetchContent (download only, `ebur128.c` built in `tools/`) | `tools/osp-render` (not shipped) | Pin checked against the v1.2.6 tag via the GitHub API on 2026-10-25 |
| dr_wav (dr_libs) | 0.14.6 → `14423ee630e4b36596d042fa055d7f1d0a02ca38` | Unlicense OR MIT-0 (dr_wav.h header) | CMake FetchContent (download only) | `tools/osp-render` (not shipped) | dr_libs has no per-library tags; `DRWAV_VERSION_*` = 0.14.6 at this commit (checked 2026-10-25) |
| nlohmann/json | v3.12.0 → `55f93686c01528224f448c19128836e7df245f72` | MIT | CMake FetchContent (`cmake/OspDependencies.cmake`) | `engine/` (PRIVATE: preset and state JSON) | Pin checked against the v3.12.0 tag via the GitHub API on 2026-10-25 |
| JUCE | 9.0.4 → `3fb5cbc0340758596dac39f7c8de47d562b4896e` | AGPL-3.0 (AGPLv3 option of the JUCE dual licence, `LICENSE.md`) | CMake FetchContent (`cmake/OspDependencies.cmake`) | `app/` (Standalone + VST3) | Pin checked against the 9.0.4 tag via the GitHub API on 2026-10-25 |
| VST3 SDK | bundled in JUCE 9.0.4 | MIT | Inside the JUCE fetch | `app/` VST3 wrapper | ADR-0006 D2 |
| ASIO headers | bundled in JUCE 9.0.4 (`modules/juce_audio_devices/native/asio/`) | GPL-3.0 (GPL option of the Steinberg dual licence) | Inside the JUCE fetch, never committed | `app/` Standalone (`JUCE_ASIO=1`) | ADR-0001 as amended by #30 |
| Signalsmith `dsp` | v1.7.3 → `ca390d28c41ea5108a528e2e81faa4088f2f8ac8` | MIT | CMake FetchContent (`cmake/OspDependencies.cmake`) | `dsp-core` cab convolution (shipped, #69) and `tests/measure` FFT | ADR-0006 Amendment A1; `LICENSE.txt` read at the pin on 2026-10-25 |

Approved dependency list, pins and licences: [ADR-0006](docs/adr/0006-build-and-dependencies.md) §D2. A row is added
here in the PR that first builds the dependency.
