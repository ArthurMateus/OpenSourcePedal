# ADR-0003: Preset format, versioning and plugin state

- Status: **Proposed** (2026-10-25). Becomes Accepted when the cross-account review approves it.
- Deciders: architect (acct1/Lyra). Issue #24.
- Rests on: `docs/research/reference-plugins.md` (§2.4 UX patterns, Rec. 7), `docs/research/nam-runtime.md` (§2.3
  `.nam` metadata, Rec. "Calibration"), `docs/research/tone-targets-a.md` / `tone-targets-b.md` (§3.3 targets),
  ADR-0006 (PR #34: nlohmann/json 3.12.0 in `engine/`). Details: [`docs/spec/preset-schema.md`](../spec/preset-schema.md),
  machine-readable schema: [`presets/schema/preset.v1.schema.json`](../../presets/schema/preset.v1.schema.json).

## Context

MASTER_PROMPT asks for factory presets plus user presets that can be created, saved, renamed, duplicated, imported and
exported as **versioned JSON with forward migration**. The reference products store presets as encrypted XML in user
folders and subfolders. They keep input mode, tuner, metronome and the MIDI map out of the preset, and they mark unsaved
changes with an asterisk (reference-plugins §2.4). The newest products add a "Global Lock" that keeps input, gate,
transpose, doubler and output while you browse presets (reference-plugins §2.4). Presets refer to user `.nam` captures
and IR WAVs that live outside the preset. The reference stores the custom IR path in the preset (reference-plugins §2.4).
A plugin host stores one opaque state blob per instance.

## Decision

1. **Format:** UTF-8 JSON (RFC 8259), file extension **`.osp.json`**, top-level `"format": "osp-preset"`. Plain text, not
   encrypted. Factory presets are diffable and lintable in CI. Written with sorted keys, 2-space indent and
   shortest-round-trip numbers (nlohmann/json defaults), so a save is byte-stable.
2. **Versioning:** an integer **`schemaVersion`** (v1 = this ADR).
   - **Additive** changes (a new optional field, a new block type, a new parameter) do **not** bump it. Readers ignore
     and **preserve** unknown fields.
   - **Breaking** changes (rename, unit or semantic change, structural move) bump it by 1 and add exactly one pure
     migration function `migrate_vN_to_vN1(json&)`.
   - The loader runs the chain `v(file) → … → v(current)` on a non-audio thread before parsing into engine types.
   - A file **newer** than the reader is loaded best-effort with a warning, marked read-only (Save As only), and never
     downgraded in place.
3. **Values are stored in engineering units** (dB, ms, Hz, %, semitones), never as normalized 0–1 host values. Enums are
   strings and switches are booleans. The parameter **string ids** from `docs/spec/parameters.md` (#23) are the keys. Then
   a range or skew change in a later release does not silently change old presets.
   - Missing parameters take their spec default.
   - Out-of-range values are clamped, with a load warning.
   - Unknown parameter ids are preserved and ignored.
4. **Structure:** an ordered **`blocks` array** of `{type, id, enabled, params, resources?, channels?}`.
   - The schema accepts any order. The v1 engine validates against the orders allowed by `docs/spec/signal-flow.md`,
     which leaves the pre/post modulation question (reference-plugins Q1) to that spec (#22) without a schema change.
   - Amp **channels** (3 by default, reference-plugins Summary 3) live in the amp block, together with `activeChannel`.
   - Blocks bound to the amp channel (EQ per amp, cab when linked, reference-plugins §2.4) use `perChannel: true` and a
     parallel `channels` array.
   - An unknown block **type** is preserved and bypassed.
5. **External resources** (`.nam`, IR) are referenced as `{uri, sha256, displayName}`.
   - `uri` is one of `factory:<id>`, `lib:<path relative to the user library>`, or `file:///<absolute path>`.
   - The loader resolves the `uri` first. If the file is missing or its hash differs, it falls back to a **hash lookup**
     in the library index. If both fail, the block loads **bypassed** with a "missing resource" warning, and the preset
     still loads.
   - Resources are never embedded in a preset, because `.nam` files can be MBs (nam-runtime §2.3).
6. **Banks:**
   - **Factory** presets ship read-only in `presets/factory/<category>/`, installed under `%ProgramData%`, and have
     stable UUIDs. They are guard-checked, so they carry original names only (AGENTS.md rule 2).
   - **User** presets live in `%APPDATA%\OpenSourcePedal\Presets\` with free folders and subfolders.
   - Saving a factory preset always means Save As into the user bank.
   - Rename, duplicate and delete are file operations. Duplicate assigns a new UUID.
7. **Not stored in presets (global settings file):** input mode (mono/stereo), tuner, metronome, MIDI mappings and
   program-change map, Global Lock on/off, UI scale, device settings and the NAM interface calibration level
   (`inputInterfaceLevel_dBu`, nam-runtime Rec.). This matches the reference behaviour (reference-plugins Rec. 7). The
   Global-Lock blocks (`input`, `gate`, `transpose`, `doubler`, `output`) **are** stored in the preset. Global Lock only
   skips applying them.
8. **Import/export:**
   - Single preset: copy the `.osp.json`.
   - **Bundle** (`.ospbundle`): a zip archive holding `manifest.json`, `presets/**.osp.json` and `resources/<sha256>.<ext>`.
     On import, the hashes are verified, resources are copied into the library (dedup by hash), `uri`s are rewritten to
     `lib:` and UUIDs collide → new UUID.
   - The zip is handled in `app/` (JUCE `ZipFile`), so `engine/` stays JUCE-free (ADR-0001, ADR-0006 D3).
9. **Plugin state blob** (`getStateInformation`): the UTF-8 JSON
   `{"format":"osp-state","stateVersion":1,"preset":<full preset object>,"source":{"presetId","dirty"},"ui":{…}}`.
   - The embedded preset uses the same migration chain.
   - Host-automatable parameters are restored from the preset object, which is the single source of truth.
   - UI state (page, window size) is restored, but is never part of a preset.
10. **Safety limits:**
    - Preset files are ≤ 1 MiB and nesting depth ≤ 16. Larger or deeper files are rejected before parsing.
    - Strings are ≤ 256 chars (description ≤ 2000).
    - Parsing, migration and resource loading never run on the audio thread. The parsed preset becomes an immutable
      engine snapshot, handed to the audio thread lock-free (threading spec, issue #25).

## Consequences

- \+ Old presets keep their sound across range changes. A migration is a small, testable pure function with golden
  fixtures (`tests/presets/migration/vN/`).
- \+ Forward compatibility: older builds can open newer additive presets, and Save does not drop unknown fields.
- \+ Plain JSON allows CI linting of factory presets (schema + guard + parameter ranges) and readable diffs in PRs.
- − Engineering units require the engine to own the unit ↔ normalized conversion for host automation (parameters.md).
- − A hash on every user resource costs one SHA-256 per file at save/import time. The cost is negligible at the sizes in
  nam-runtime §2.3.
- − Plain text makes factory presets easy to copy. That is acceptable for an AGPL project.

## Deferred (with owner)

| Question | Owner |
|---|---|
| Allowed block orders (modulation pre/post, second drive slot) | `docs/spec/signal-flow.md` (#22) |
| Exact parameter ids, ranges and defaults | `docs/spec/parameters.md` (#23) |
| Lock-free snapshot handoff and loader thread | threading spec / ADR (#25) |
| Factory preset categories and names | preset-designer (Phase 4) |
| Embedding resources in the plugin state for project portability | revisit after Phase 3 user feedback |
