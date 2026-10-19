# Developing MIDI Workbench

For someone changing the code. Build instructions are in [BUILDING.md](BUILDING.md); binding rules
in [../AGENTS.md](../AGENTS.md); verified state in [../IMPLEMENTATION_STATUS.md](../IMPLEMENTATION_STATUS.md).

## Layout

```
include/mwb/<module>/   every header (no public/private split; internal ones in detail/)
src/<module>/           .cpp only; executable entry points in src/<module>/main/
tests/<area>_tests.cpp  doctest suites; tests/fuzz/ libFuzzer targets and seeds
deps/                   vendored libraries + manifest.json + update_deps.py
patches/<name>/         recorded patches to vendored libraries
cmake/                  module helper, install/packaging, third-party notices
tools/                  include-graph check, icon renderer
docs/                   these documents and images
SKILLS/<task>/SKILL.md  step-by-step task guides
```

## Modules and dependency direction

Each module is a static library `mwb::<name>` declared in `CMakeLists.txt` with the only modules
it may include. The `include-graph` test fails on any other include.

```
core ──────────────────────────────────────────────── ids, time, events, model, note pairing,
 │                                                     tempo/meter maps, change sets, hashing
 ├─ midi        SMF/RMID import/export (libremidi adapter, reports)
 ├─ music       scales, chords, progressions, transformations, analysis, fingerprints, voices
 ├─ playback    snapshots, scheduler, transport state machine, controller chase
 ├─ jobs        bounded BS::thread_pool coordinator
 ├─ document    (core, midi) native .mwb, command history, working session, recovery journal
 ├─ search      (core, music) query grammar, SQL compiler, browse index, motif search
 ├─ library     (core, midi, music, document, search) catalog, objects, import, revisions, backup
 ├─ audio       (core, playback) ISynth, built-in synth, SoundFont, block renderer, WAV render
 ├─ platform    (core, audio, playback) WASAPI/null output, audio engine, WinMM/null MIDI, watcher
 ├─ worker      (core, midi, music, document, playback, audio) IPC protocol, mwb-worker service (MIDI parsing, SF2 validation), worker client
 ├─ app         (all of the above) file I/O, data directories, logging, recording logic
 ├─ cli         (app and below) mwb-cli commands
 └─ ui          (app and below, Qt) main window and editors
```

Executables: `MIDIWorkbench.exe` (ui), `mwb-cli.exe` (cli), `mwb-worker.exe` (worker).

## The document model and edits

- `DocumentData` → `Sequence` (division, settings, regions, scale regions, recipes) → `Track`
  (events sorted by `(tick, order_key)`). Raw events are authoritative; notes are a projection
  (`pair_notes`, FIFO per port/channel/pitch; a note's ID is its note-on event ID).
- Every edit is a `ChangeSet` of reversible operations (`core/change_set.hpp`). `apply_change_set`
  validates and applies all operations or reverts to the original state; `invert` yields the undo.
  Build change sets with `ChangeBuilder` (`insert`, `remove`, `replace`, `add_note`, `set_events`)
  or the higher-level helpers: `core/note_edit.hpp` (editor note edits) and the music module's
  tools (`music/transform.hpp`, which all return a `TransformPreview` = change set + report).
- `document::WorkingSession` owns the document, the `CommandHistory` (256 MiB budget, state
  identities for dirty tracking), the file path and save tickets. `connect_recovery` journals every
  transition to the recovery database.
- `semantic_hash(document)` is the test oracle for "undo restores the exact state".

## How responsiveness works

- The GUI applies edits immediately on its thread (they are small and bounded); playback snapshots
  compile on a dedicated thread (`ui/playback_service.cpp`) and only the newest request survives.
- The audio render callback (`platform::AudioEngine::render`) never allocates, locks or logs.
  Snapshots arrive through `RetiringHandoff`; retired ones are destroyed by the GUI-side owner.
- Library work (open, import, analyze, search, metadata) runs on one background thread
  (`ui/library_panel.cpp`); results return with `QMetaObject::invokeMethod` and are dropped when a
  newer search generation exists. Searches are debounced by 120 ms.
- Parsing of untrusted files can run in `mwb-worker.exe` under a Job Object (`worker/worker.hpp`).
- UI timers only sample display state (transport position at 30 Hz, recovery flush every 2 s,
  MIDI input drain every 4 ms); musical events are never sent from a Qt timer.

## Tests

`ctest --preset debug` runs one CTest entry per doctest `TEST_SUITE` plus `include-graph`. Areas:
`core`, `midi`, `music`, `document`, `search`, `library`, `playback`, `audio`, `platform`, `jobs`,
`worker`, `app`, `cli`, `ui`. GUI tests run offscreen. Fuzz targets: see BUILDING.md (`fuzz`
preset) and `tests/fuzz/`; run one with
`build/fuzz/tests/fuzz/mwb_fuzz_smf_import.exe -max_total_time=60 build/fuzz/corpus/smf_import`.

## CI

`.github/workflows/ci.yml`: a lint job (`update_deps.py --check`) and a Windows matrix
(`ci`, `ci-sanitize`, `debug`) that prints tool versions, builds with `-k 0`, runs CTest, installs,
and starts the installed CLI and GUI with only System32 on `PATH`. Action versions are pinned by
commit SHA. The workflow has not been run on GitHub from this tree yet (no repository exists).
