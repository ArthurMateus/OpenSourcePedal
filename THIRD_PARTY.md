# Third-party software

Every dependency must be AGPLv3-compatible. Add a row in the same PR that introduces the dependency.

| Component | Version / pin | License | How obtained | Used in | Notes |
|---|---|---|---|---|---|
| Catch2 | v3.16.1 → `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | CMake FetchContent (`cmake/OspDependencies.cmake`) | `tests/` only (not shipped) | Pin checked against the v3.16.1 tag via the GitHub API on 2026-10-25 |

Approved dependency list, pins and licences: [ADR-0006](docs/adr/0006-build-and-dependencies.md) §D2. A row is added
here in the PR that first builds the dependency.
