# ADR-0001: Project license AGPL-3.0-or-later; framework JUCE 8

- Status: **Accepted** (2026-10-25). The Phase 1 research note `docs/research/asio-low-latency.md` and
  `docs/research/framework-comparison.md` re-verify the terms. If they find a conflict, supersede this ADR.
- Deciders: orchestrator (acct2), confirmed by the owner during bootstrap.

## Context

We need a standalone Windows app with first-class ASIO, a VST3 plugin, `pluginval` compatibility and a mature
GUI toolkit. The owner is a C++/DSP developer. The DSP core must stay framework-free.

Facts (checked 2026-10-25):
- **JUCE 8** is dual-licensed: AGPLv3 or the commercial JUCE 8 EULA. Source: JUCE `LICENSE.md`;
  https://forum.juce.com/t/amendments-to-the-juce-end-user-licence-agreement-for-juce-8/61265 ; https://en.wikipedia.org/wiki/JUCE
- **ASIO SDK**: since 2025-10-15, Steinberg offers it under **GPLv3** in addition to the proprietary license.
  Sources: https://heise.de/-10963451 ; https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/
- **VST 3 SDK 3.8** is MIT-licensed (2025-10-29). Source: Steinberg press release, 2025-10-29.
- **iPlug2** is permissive (zlib-like), but its standalone app and ASIO device handling are less mature (unverified,
  to be checked in Phase 1 research).

## Decision

- License the whole project under **AGPL-3.0-or-later**.
- Use **JUCE 8** (open-source AGPLv3 option) for the app, plugin wrappers and UI only. `dsp-core/` has no JUCE dependency.
- Use the ASIO SDK under its GPLv3 option. GPLv3 and AGPLv3 may be combined (AGPLv3 §13 / GPLv3 §13).
  We **fetch the SDK at configure time** (CMake `FetchContent`, pinned URL + hash) and never commit it. That keeps the repo
  clean even if the terms change again.

## Consequences

- + Most mature path to standalone + ASIO + VST3 + pluginval. Large community, good docs.
- + No licence fee. No splash-screen requirement under the AGPL option.
- − Downstream users must comply with the AGPL (source availability, including network use).
- − Third-party code must be AGPLv3-compatible. GPLv2-only code is excluded.
- `dsp-core/` is still our own code, so a later relicence of that module alone (for example, for a hardware port)
  is possible, as long as every contributor is an agent of this project.
