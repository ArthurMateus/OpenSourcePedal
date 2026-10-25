# ADR-0001: Project license AGPL-3.0-or-later; framework JUCE 9

- Status: **Accepted** (2026-10-25). **Amended** 2026-10-25 after the Phase 1 research
  (`docs/research/asio-low-latency.md`, #16, and `docs/research/framework-comparison.md`, #20): the framework pin
  moves from JUCE 8 to **JUCE 9.0.4**, and ASIO comes from **JUCE's bundled headers** instead of a configure-time SDK fetch.
- Deciders: orchestrator (acct2), confirmed by the owner during bootstrap.

## Context

We need a standalone Windows app with first-class ASIO, a VST3 plugin, `pluginval` compatibility and a mature
GUI toolkit. The owner is a C++/DSP developer. The DSP core must stay framework-free.

Facts (checked 2026-10-25):
- **JUCE 8 and JUCE 9** are dual-licensed: AGPLv3 or the commercial JUCE 8 EULA. Source: JUCE `LICENSE.md`;
  https://forum.juce.com/t/amendments-to-the-juce-end-user-licence-agreement-for-juce-8/61265 ; https://en.wikipedia.org/wiki/JUCE
- **ASIO SDK**: since 2025-10-15, Steinberg offers it under **GPLv3** in addition to the proprietary license.
  Sources: https://heise.de/-10963451 ; https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/
- **VST 3 SDK 3.8** is MIT-licensed (2025-10-29). Source: Steinberg press release, 2025-10-29.
- **iPlug2** is permissive (zlib-like), but its standalone app and ASIO device handling are less mature (unverified,
  to be checked in Phase 1 research).

## Decision

- License the whole project under **AGPL-3.0-or-later**.
- Use **JUCE 9** (open-source AGPLv3 option), pinned to **9.0.4** (2026-10-12). The 8.x line has been security-only since
  2026-07-21, and JUCE 9's open-source terms are unchanged from JUCE 8 (#20). JUCE is used for the app, plugin wrappers
  and UI only. `dsp-core/` has no JUCE dependency. Future minor bumps go through ADR-0006 (build and dependencies).
- ASIO: use the three **ASIO SDK 2.3.4 headers that JUCE bundles** (since 8.0.11) under the SDK's **GPL-3.0-only** option,
  via `JUCE_ASIO=1`. GPLv3 and AGPLv3 may be combined (§13 of each). No configure-time SDK fetch is needed, and the SDK is still
  never committed to this repo. ASIO-enabled binaries therefore contain GPL-3.0-only code (#16 §2.1).
- CLAP (optional, later): clap-juce-extensions (MIT), because JUCE 9.0.4 has no native CLAP (#20).

## Consequences

- + Most mature path (JUCE 9 is actively maintained) to standalone + ASIO + VST3 + pluginval. Large community, good docs.
- + No licence fee. No splash-screen requirement under the AGPL option.
- − Downstream users must comply with the AGPL (source availability, including network use).
- − Third-party code must be AGPLv3-compatible. GPLv2-only code is excluded.
- `dsp-core/` is still our own code, so a later relicence of that module alone (for example, for a hardware port)
  is possible, as long as every contributor is an agent of this project.
