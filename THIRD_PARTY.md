# Third-party software

Every dependency must be AGPLv3-compatible. Add a row in the same PR that introduces the dependency.

| Component | Version / pin | License | How obtained | Used in | Notes |
|---|---|---|---|---|---|
| Catch2 | v3.16.1 → `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | CMake FetchContent (`cmake/OspDependencies.cmake`) | `tests/` only (not shipped) | Pin checked against the v3.16.1 tag via the GitHub API on 2026-10-25 |
| libebur128 | v1.2.6 → `67b33abe1558160ed76ada1322329b0e9e058b02` | MIT | CMake FetchContent (download only, `ebur128.c` built in `tools/`) | `tools/osp-render` (not shipped) | Pin checked against the v1.2.6 tag via the GitHub API on 2026-10-25 |
| dr_wav (dr_libs) | 0.14.6 → `14423ee630e4b36596d042fa055d7f1d0a02ca38` | Unlicense OR MIT-0 (dr_wav.h header) | CMake FetchContent (download only) | `tools/osp-render` (not shipped) | dr_libs has no per-library tags; `DRWAV_VERSION_*` = 0.14.6 at this commit (checked 2026-10-25) |

Approved dependency list, pins and licences: [ADR-0006](docs/adr/0006-build-and-dependencies.md) §D2. A row is added
here in the PR that first builds the dependency.
