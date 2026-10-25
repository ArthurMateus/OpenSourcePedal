# Third-party software

Every dependency must be AGPLv3-compatible. Add a row in the same PR that introduces the dependency.

| Component | Version / pin | License | How obtained | Used in | Notes |
|---|---|---|---|---|---|
| Catch2 | v3.16.1 → `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | CMake FetchContent (`cmake/OspDependencies.cmake`) | `tests/` only (not shipped) | Pin checked against the v3.16.1 tag via the GitHub API on 2026-10-25 |
| JUCE | 9.0.4 → `3fb5cbc0340758596dac39f7c8de47d562b4896e` | AGPL-3.0 (AGPLv3 option of the JUCE dual licence, `LICENSE.md`) | CMake FetchContent (`cmake/OspDependencies.cmake`) | `app/` (Standalone + VST3) | Pin checked against the 9.0.4 tag via the GitHub API on 2026-10-25 |
| VST3 SDK | bundled in JUCE 9.0.4 | MIT | Inside the JUCE fetch | `app/` VST3 wrapper | ADR-0006 D2 |
| ASIO headers | bundled in JUCE 9.0.4 (`modules/juce_audio_devices/native/asio/`) | GPL-3.0 (GPL option of the Steinberg dual licence) | Inside the JUCE fetch, never committed | `app/` Standalone (`JUCE_ASIO=1`) | ADR-0001 as amended by #30 |

Approved dependency list, pins and licences: [ADR-0006](docs/adr/0006-build-and-dependencies.md) §D2. A row is added
here in the PR that first builds the dependency.
