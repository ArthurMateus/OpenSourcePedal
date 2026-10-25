# Preset schema (v1)

- Decision record: [ADR-0003](../adr/0003-preset-schema.md). Machine-readable schema:
  [`presets/schema/preset.v1.schema.json`](../../presets/schema/preset.v1.schema.json) (JSON Schema draft 2020-12).
  Worked example: [`presets/schema/examples/example.osp.json`](../../presets/schema/examples/example.osp.json).
- Parameter ids, units and ranges: `docs/spec/parameters.md` (#23). Allowed block orders: `docs/spec/signal-flow.md`.

## 1. Files and locations

| What | Where | Writable |
|---|---|---|
| Factory presets (source) | `presets/factory/<category>/<slug>.osp.json` in the repo | CI-linted |
| Factory presets (installed) | `%ProgramData%\OpenSourcePedal\Presets\Factory\` | No |
| User presets | `%APPDATA%\OpenSourcePedal\Presets\User\` (free folders and subfolders) | Yes |
| User library (captures, IRs) | `%APPDATA%\OpenSourcePedal\Library\` plus `library-index.json` (sha256 → path) | Yes |
| Global settings | `%APPDATA%\OpenSourcePedal\settings.json` (own `settingsVersion`) | Yes |

File names are derived from the preset name (slug), but the **`id` (UUID v4) is the identity**. Renaming a preset renames
the file and keeps the id.

## 2. Top-level object

| Field | Type | Req. | Notes |
|---|---|---|---|
| `format` | `"osp-preset"` | ✔ | Rejects foreign JSON early |
| `schemaVersion` | integer | ✔ | `1`. See §5 |
| `id` | UUID v4, lowercase | ✔ | Stable across renames. Duplicate/import-collision → new UUID |
| `name` | string 1–64 | ✔ | Original names only (guard + linter) |
| `author`, `category` | string ≤ 256 | | Category drives the browser folders for factory presets |
| `tags` | string[] ≤ 32, each ≤ 32 | | Search and filter in the browser (reference-plugins §2.4 "Preset Explorer") |
| `description` | string ≤ 2000 | | |
| `createdWith` | semver | | App version that last saved the file. Used for diagnostics, never for logic |
| `tempoBpm` | 20–300 | | Used by synced delays when the host gives no tempo |
| `blocks` | block[] 1–32 | ✔ | Ordered signal chain. See §3 |

## 3. Blocks

```jsonc
{ "type": "delay", "id": "delay", "enabled": true,
  "params": { "mode": "tape", "sync": "1/4", "feedbackPct": 30.0, "mixPct": 25.0 } }
```

- `type`: v1 types are `input gate comp drive amp cab eq mod delay reverb transpose doubler output`. `id` is unique in
  the preset. It is the block-type name, plus a digit if a type appears twice (`drive1`, `drive2`). The full parameter id
  is `<id>.<key>`, e.g. `delay.feedbackPct`. Keys may carry one or two dotted sub-levels (`ir1.levelDb`).
- `params`: values in **engineering units** (dB, ms, Hz, %, st, s). Enums are strings and switches are booleans. Never
  normalized 0–1.
- `resources`: named external files, e.g. `{"ir1": <resourceRef>, "ir2": <resourceRef>}` for the two cab slots.
- **Amp:** `activeChannel` (0-based) plus `channels[]` (1–4, 3 by default). Each channel holds `{name?, model, params,
  resources?}`. `model` is either `{"kind":"builtin","id":"osp.amp.<name>"}` or `{"kind":"nam","capture":<resourceRef>}`.
- **Per-channel blocks:** `perChannel: true` plus `channels[]` of `{params, resources?}`, index-aligned with the amp
  channels. Used for the EQ (one EQ per amp, as in the reference) and for the cab when "link" is on. When the amp
  switches channel, these blocks switch with it, so a channel change from MIDI needs no preset reload.
- `enabled: false` = bypassed. The block's state is still stored.

## 4. Resource references

`{ "uri": "...", "sha256": "<64 hex>", "displayName": "..." }`. `sha256` is required unless `uri` is `factory:`.

| `uri` scheme | Meaning |
|---|---|
| `factory:<id>` | Bundled asset (row in `assets/LICENSES.md`), e.g. `factory:ir/closed-4x12-a` |
| `lib:<relative path>` | Inside the user library. Portable between machines that share the library layout |
| `file:///<absolute>` | Anywhere else. Not portable. The hash is the fallback |

**Resolution (loader thread):** (1) resolve `uri`. If the file exists and its hash matches, use it. (2) Otherwise look up
`sha256` in `library-index.json`. If found, use that path and mark the preset dirty with a "path updated" note. (3)
Otherwise the owning block or channel loads **bypassed** with a visible "missing: <displayName>" warning. The rest of the
preset loads normally. Hashing is SHA-256 over the raw file bytes.

## 5. Versioning and migration

- **Additive change** (new optional field, block type, parameter or enum value): **no** version bump. Old readers ignore
  the new content **and keep it**. Parsed presets carry the original JSON, so an edit-and-save round-trips unknown keys.
- **Breaking change**: bump `schemaVersion`, add `presets/schema/preset.vN.schema.json`, and add one migration:

```cpp
// engine/src/presets/migrations.cpp — pure, no I/O, runs on the loader thread
using Migration = void (*)(nlohmann::json& preset);   // vN -> vN+1, sets schemaVersion = N+1
constexpr std::array<Migration, kCurrentSchema - 1> kMigrations = { /* migrate_v1_to_v2, ... */ };
```

- Every migration ships with fixtures `tests/presets/migration/vN/<case>.in.json` and `<case>.expected.json`. The test
  runs the whole chain from every historic version.
- **Newer file than the reader** (`schemaVersion > kCurrentSchema`): load best-effort (known blocks and params only),
  show "made with a newer version", and disable Save. Save As writes the current version.

## 6. Load pipeline and error policy

| Condition | Behaviour |
|---|---|
| > 1 MiB, nesting > 16, invalid UTF-8/JSON, `format` ≠ `osp-preset` | Reject, show an error, keep the current preset |
| Schema violation in a known field | Reject (factory presets fail CI instead) |
| Unknown top-level field, block field or parameter key | Keep, ignore |
| Unknown block `type` | Keep, run as bypass (passes audio through) |
| Missing parameter | Spec default from `parameters.md` |
| Out-of-range numeric value | Clamp, warning in the load report |
| Unknown enum string | Spec default, warning |
| Missing resource | See §4 step 3 |

Parsing, migration, hashing and model/IR loading run on the loader thread. The result is an immutable snapshot handed to
the audio thread lock-free (threading spec, #25). The audio thread never touches JSON.

## 7. Not in presets (global `settings.json`)

Input mode (mono/stereo), tuner, metronome, MIDI mappings and program-change map, **Global Lock** state, UI scale,
audio device, and the NAM interface calibration `inputInterfaceLevel_dBu` (nam-runtime Rec. "Calibration"). This matches
the reference (reference-plugins Rec. 7). With Global Lock on, a preset load **skips** the blocks `input`, `gate`,
`transpose`, `doubler` and `output`. Their stored values stay in the file.

## 8. Plugin state blob

`getStateInformation()` writes UTF-8 JSON (no XML wrapper):

```json
{ "format": "osp-state", "stateVersion": 1,
  "preset": { "...": "full preset object, same schema" },
  "source": { "presetId": "3b0e…0f13", "dirty": true },
  "ui": { "page": "amp", "width": 1280, "height": 800 } }
```

`setStateInformation()` runs the same pipeline (§6) on the embedded preset. The `dirty` flag drives the "*" marker.
Host-visible parameters are reported back to the host from the restored values.

## 9. Import / export bundle (`.ospbundle`)

A zip file containing `manifest.json`, `presets/<…>.osp.json` and `resources/<sha256>.<ext>`:

```json
{ "format": "osp-bundle", "bundleVersion": 1, "createdWith": "0.3.0",
  "presets": ["presets/Glass Clean.osp.json"],
  "resources": [{ "sha256": "9f2c…c5d6", "file": "resources/9f2c…c5d6.nam", "displayName": "My lead capture" }] }
```

- **Export** rewrites each user resource `uri` to `lib:Imported/<sha256>.<ext>`.
- **Import** verifies every hash, dedups resources into `Library\Imported\`, gives a new UUID on an id collision, and
  never overwrites a user preset (it appends " (2)" to the name).
- Zip handling lives in `app/` (JUCE `ZipFile`). `engine/` sees extracted files only.

## 10. Requirements (each maps to a test)

| ID | Requirement | Test |
|---|---|---|
| REQ-PRE-001 | Every factory preset validates against `preset.v1.schema.json` and passes the guard | CI lint (`tools/`) |
| REQ-PRE-002 | Load → save without edits is byte-identical for files written by the current version | `osp_tests` round-trip over the factory bank |
| REQ-PRE-003 | Unknown fields, block types and param keys survive load → edit → save | Unit test with a fixture carrying extras |
| REQ-PRE-004 | The migration chain turns every `tests/presets/migration/vN` input into its expected output | Golden-fixture test |
| REQ-PRE-005 | A preset with a missing resource loads, with only the owning block/channel bypassed | Unit test with a dangling `lib:` uri |
| REQ-PRE-006 | A resource moved inside the library is found by its hash | Unit test with an index lookup |
| REQ-PRE-007 | Files > 1 MiB or nesting > 16 are rejected without a crash. The fuzzed preset loader never crashes or hangs | Fuzz test (ADR-0007) |
| REQ-PRE-008 | Out-of-range values are clamped to the `parameters.md` range | Unit test |
| REQ-PRE-009 | Plugin state save → restore reproduces the audio output (null ≤ −120 dBFS) | Golden-render state round-trip (pluginval + `osp_tests`) |
| REQ-PRE-010 | With Global Lock on, loading a preset leaves `input`/`gate`/`transpose`/`doubler`/`output` unchanged | Unit test |
| REQ-PRE-011 | No JSON parsing, file I/O or allocation from preset handling happens on the audio thread | RT checker (`realtime-rules.md`) |
