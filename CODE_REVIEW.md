# MIDI Workbench code review, round 1

| | |
|---|---|
| Date | 2026-10-19 |
| Scope | Owned code: `include/mwb/**`, `src/**`, `tests/**`, `cmake/**`, `CMakeLists.txt`, `CMakePresets.json`, `deps/update_deps.py`, `deps/CMakeLists.txt`, `tools/*.py`. Vendored code under `deps/<library>/` was read only to see how the project uses it. |
| Method | Specification section 34.7. Eight parallel read-only reviewers each read one slice in full: core+midi, music, document+jobs+worker, search, library, playback+audio+platform, ui, and app+cli+build/CI/docs. The lead reviewer re-opened every Critical and High finding and traced its code path before including it. |
| Yardstick | The `AGENTS.md` rules, memory safety, performance (a hot-path finding rates one level higher), C++ standard conformance, and readability. |
| Tree state | **The tree was being edited during the review.** `src/ui/main_window.cpp`, `src/ui/recording_controller.cpp`, `src/app/recording.cpp`, `src/core/change_set.cpp` and `src/document/command_codec.cpp` changed at 15:06–15:09. `docs/DEVELOPING.md` and `SKILLS/*` appeared at 15:15, and `IMPLEMENTATION_STATUS.md` at 15:25. Line numbers in `main_window.cpp` may have drifted. The lead reviewer re-read them at about 15:30. |

**What was actually run.** These are the only commands the review executed. They used a private build directory and did not touch `build/debug`, `build/release`, `build/sanitize` or `build/fuzz`.
- `cmake --preset debug -B build/review -DMWB_WERROR=ON`, then `cmake --build build/review -- -k 0`: 205/205 steps, **0 warnings**, exit 0. This was the tree as it stood at about 15:12.
- `ctest --test-dir build/review -j4` with `QT_QPA_PLATFORM=offscreen` and null audio/MIDI backends: **`100% tests passed out of 55`**.
- `python deps/update_deps.py --check`: `checked 24 dependencies: OK`.

Not run: the fuzz targets, the sanitize preset, the release preset, benchmarks, and the GUI interactively. Every performance cost below that is not quoted from `docs/testing/*.json` is an estimate, and is labelled as one.

---

## Overall score: **5 / 10**

The foundations are better than the score suggests:
- The audio render path is real-time clean: no allocation, locks, logging, frees or Qt calls on the callback (traced in full).
- Change sets commit completely or not at all.
- The native file loader is hardened properly: `sqlite3_limit`, defensive mode, `trusted_schema=OFF`, an exact schema whitelist, and an atomic save.
- The query language uses PEGTL and the SQL is fully parameterized.
- The build is warning-clean under `-Werror`, and all 55 CTest entries pass.

Two systemic problems pull the score down.

1. **Whole-document work on the GUI thread for every edit.** One nudge or keystroke edit triggers all of the following:
   - a full SHA-256 of the document, plus periodic full native serialization;
   - a deep copy of the document for playback;
   - an O(N log N) rebuild of the event table, also triggered by every edit-cursor move;
   - a rebuild of the note pairing and index;
   - a full-track scan in every controller lane paint;
   - in core note editing, a linear `find_event` plus vector erase/insert per note, which is quadratic.

   `docs/testing/bench-e100k.json` already records 78 ms to prepare a 3,125-note move. That cost grows quadratically.
2. **Untrusted input reaches throwing or unbounded code in-process.** Specifically:
   - nlohmann `at()`/`value()`/`dump()` throw on bad clipboard data, bad worker output, and invalid UTF-8 names;
   - SMF import limits are checked only after full materialization;
   - recipe parameters reach unchecked `int64` arithmetic;
   - the worker-process isolation exists but is not used.

There are 19 High findings, most of them easy to fix one at a time, and no Critical finding.

---

## Per-module scorecard

| Module | Score | Previous round | Reason |
|---|---|---|---|
| core | 6 | n/a | Change-set atomicity is sound and tested, and tempo math uses exact 128-bit arithmetic. But note editing is quadratic on the GUI thread (two Highs), `Rational::make` can negate `INT64_MIN`, and `note_edit` has no core tests. |
| midi | 6 | n/a | Uses patched libremidi for reading and writing, as required, and has fuzz targets. But event limits are checked only after a whole track is converted (OOM, and `bad_alloc` escapes a `Result` API), and the writer's end-of-track heuristic can drop the EOT event. |
| music | 7 | n/a | A pure editor with hash lookups, exact rational grids, pcg PRNG and non-throwing JSON, plus good golden tests. Tool parameters have no upper bounds, so recipe values cause signed-overflow UB, and some paths are super-linear (analysis per track, split, voice-leading DP). |
| document | 7 | n/a | Strong untrusted-load path, atomic save and checksummed journal. The recovery listener hashes the whole document per edit and serializes checkpoints on the GUI thread. The journal never resyncs after one failed record, and replay skips `validate_document`. |
| search | 7 | n/a | Real PEGTL grammar with limits, parameterized SQL, keyset cursors, and a carefully bounded stream decoder. Approximate-search voting is unbounded. `BrowseIndex::page` re-sorts everything per call, and the in-memory index is not wired into production. |
| library | 6 | n/a | Matches section 22.5: WAL, one writer, a bounded reader pool, `foreign_keys`, `synchronous=FULL`, defensive mode, the backup API and keyset paging. But a UTF-8 truncation bug makes `dump()` throw and blocks renames. There is per-row statement preparation (N+1), cancelled analysis is recorded as failed, and orphan objects are never collected. |
| playback | 8 | n/a | Owned-note registry, snapshot reconciliation by identity, and a correct retiring handoff (frees happen on the owner thread). Loop wraps and held-note chase are unbounded work that the preflight budget does not count. |
| audio | 8 | n/a | The synths are RT-clean: TinySoundFont voices and channels are preallocated and voice pools are fixed. Offline WAV goes through dr_wav to a temp file and is then replaced atomically. Minor: a non-ASCII temp-name issue, and SF2 is untested through the engine. |
| platform | 7 | n/a | The callback and MIDI input are RT-safe, and `_WIN32` code has a POSIX branch. But a full external queue drops note-offs, raw input bypasses the ignore filters, and stale commands survive a close/open. |
| jobs | 5 | n/a | Small, and uses BS::thread_pool. The destructor has a use-after-reset race, `cancel_all` misses evicted running jobs, and nothing in the product uses the module. |
| worker | 5 | n/a | Sound framing (magic, version, 1 MiB cap, XXH3) and a Job Object memory limit. But it is never used, `json::value()` exceptions cross IPC, msgpack decoding has no depth limit, and the receive queue is unbounded. |
| app | 7 | n/a | Recording logic is well tested and there are no threading hazards. But take chunks can be written in an order that their own decoder rejects, and loop recording keeps the incomplete pass, which contradicts `IMPLEMENTATION_STATUS.md` item 9. |
| cli | 6 | n/a | CLI11 bindings are safe and exit codes are tested. But `dump()` throws on invalid UTF-8 with no exception barrier, there are five throwing `json::parse` calls, and `--notes` has signed overflow. |
| ui | 4 | n/a | Nine Highs. Every edit and cursor move fans out into full rebuilds and copies. There are three edit-correctness bugs, a crash from clipboard data, GUI-thread SQLite and file I/O, and a library panel that blocks during imports. Most widgets have no tests. |
| build / CI / tests / docs | 6 | n/a | Presets, `-Werror` scope, `SYSTEM` vendoring, the manifest check and the suite guard are solid. But fuzzing never runs in CI, the include-graph checker has bypasses and can silently vanish, two tests skip silently, and some doc claims are false (miniz ZIP backups, the "last complete pass"). |

---

## Fix-first table

| # | Severity | file:line | Problem |
|---|---|---|---|
| 1 | High | `src/ui/clipboard.cpp:52-53` | `value.at("tracks")`/`at("notes")` throw on foreign clipboard data. The exception escapes a Qt slot and the process terminates. |
| 2 | High | `src/cli/cli.cpp:208` | `json.dump(2)` throws on invalid UTF-8 from a `.mwb` name, and there is no `try`, so `mwb-cli inspect` aborts. |
| 3 | High | `src/library/library.cpp:38-40` (`:320-321`) | Byte truncation splits UTF-8, so `dump()` throws and every title or description edit on long CJK or Hebrew text fails. |
| 4 | High | `src/ui/main_window.cpp:808-836` | `confirm_close` saves `current_tab()`, not the tab being closed, and `close_tab` reuses a stale `index` after the modal. Unsaved edits can be lost. |
| 5 | High | `src/ui/inspector.cpp:172, 443-460` | Focus-out of Velocity writes the first note's velocity to every selected note. |
| 6 | High | `src/ui/piano_roll.cpp:1172-1180` | `set_origin`/`set_top_pitch` do not emit `mapping_changed`, so controller lanes edit at stale ticks after a scroll-bar scroll. |
| 7 | High | `src/jobs/jobs.cpp:77-80, 134-139` | The destructor returns while a task is still calling `dispatch()` through `implementation_`, which `unique_ptr::reset` has already nulled. |
| 8 | High | `src/midi/codec.cpp:225, 385-387` | `max_events` is checked only after a full track is materialized, and `bad_alloc` escapes `import_midi`. |
| 9 | High | `src/worker/*`, `src/jobs/*` (no callers) | Untrusted SMF is parsed in the GUI process (`app/document_io.cpp:109`, `library/library.cpp:625, 1618, 2292`), so the section 28.2 isolation is not applied. |
| 10 | High | `src/music/transform_time.cpp:215, 228-230, 576, 583` (and others) | Unbounded recipe `ticks`/`pitch_step` cause signed `int64`/`int` overflow (UB). |
| 11 | High | `src/core/note_edit.cpp:57-102` + `src/core/change_set.cpp:400-405, 486-505` | `update_notes` does a linear `find_event` plus erase/insert per note: O(k·n) on the GUI thread. |
| 12 | High | `src/core/note_edit.cpp:164-170` + `change_set.cpp:461-514` | `duplicate_notes`/`insert_notes` do two mid-vector inserts per note: O(k·n). |
| 13 | High | `src/document/recovery.cpp:213, 220-235` | A full-document `semantic_hash` on every edit, plus `native_to_bytes` checkpoints, run synchronously on the GUI thread. |
| 14 | High | `src/ui/main_window.cpp:641-644` + `src/ui/playback_service.cpp:206` | Every edit deep-copies the whole `DocumentData` for playback, even while stopped. |
| 15 | High | `src/ui/event_list.cpp:103-104` | Full `beginResetModel` plus `stable_sort` on every `document_changed` **and** every `view_state_changed` (each ruler-scrub mouse move), even while the dock is hidden. |
| 16 | High | `src/ui/controller_lane.cpp:143-160` | Every paint walks every event of the track and builds a polyline of all of them. |
| 17 | High | `src/ui/library_panel.cpp:80, 342-347` | One serial task thread runs import plus analysis, so search, open and metadata wait minutes during an import. |
| 18 | High | `src/ui/main_window.cpp:188-189, 857-866` + `document_controller.cpp:42-55` | GUI-thread SQLite fsync every 2 s, plus open, save and export on the GUI thread (spec 19.4). |
| 19 | High | `src/search/motif_match.cpp:565-586` | Approximate search reads every gram up to 200k postings into nested `std::map` before applying any budget. |
| 20 | Medium | `src/playback/scheduler.cpp:197-209, 372-385` | Render thread: held-note chase is O(notes), and loop wraps per block are unbounded and not counted in the preflight. |
| 21 | Medium | `src/platform/audio_engine.cpp:54-55` | A full external queue drops note-offs from stop or panic, which leaves notes stuck on hardware. |
| 22 | Medium | `src/worker/protocol.cpp:83, 116-119`; `worker_client.cpp:262, 298, 308` | `json::value()` throws across IPC, msgpack decoding has no depth limit, and the receive deque is unbounded. |
| 23 | Medium | `.github/workflows/ci.yml` | No fuzz job: the 8 libFuzzer targets are never built or run in CI. |

---

## Findings by severity

### Critical

None.

### High

Each High finding below was re-opened and traced by the lead reviewer. "Verified" means the code path was traced to the effect. "Possible" means part of the argument (a timing window, or a cost estimate) was not reproduced.

**H1. [High] `src/ui/clipboard.cpp:52-53` (also `:44-49, :82-83`). Verified.**
- **Defect.** `clipboard_from_json` checks `is_discarded`/`is_object`, then calls `value.at("tracks")` and `value.at("notes")`. These throw `out_of_range` when the key is missing. `value.value("ppq", 0)` and `scale->value("root", 0)` throw `type_error` when the type is wrong.
- **Scenario.** Any program puts `{"format":"mwb.selection","schema_version":1,"ppq":480}` on the clipboard under the app's MIME type. Ctrl+V then throws out of a QAction slot, and Qt terminates the process.
- **Fix.** Use `find()` plus `is_array()`/`is_number_integer()` checks. Add tests with missing keys and wrong types.

**H2. [High] `src/cli/cli.cpp:208`; `src/document/native_format.cpp:385-390`. Verified.**
- **Defect.** `column_text` accepts any SQLite TEXT without UTF-8 validation. `output.json.dump(2)` uses the strict handler and throws `type_error.316`. `run()` only catches CLI11 parse errors (`cli.cpp:156-166`).
- **Scenario.** `mwb-cli inspect foreign.mwb` with an invalid-UTF-8 track name hits `std::terminate`. There is no JSON output and none of the documented exit codes. The same applies to `validate`, `convert` and `library search` (`library_commands.cpp:76`).
- **Fix.** `dump(2, ' ', false, error_handler_t::replace)`. Add a `try`/`catch (const std::exception&)` barrier in `run()` that returns the internal-error envelope. Also consider validating UTF-8 in the native loader.

**H3. [High] `src/library/library.cpp:38-40`, used at `:1129, 1133, 1156-1157` and dumped at `:320-321`. Verified.**
- **Defect.** `truncate_for_change` cuts at 256 bytes, which can split a multi-byte character. `record_change` then calls `dump()` with the strict handler, which throws. `guarded` converts that to `internal`, and the edit rolls back.
- **Scenario.** Every `set_title`/`set_description` fails when the old or new text is long multi-byte text (for example 86+ CJK characters, or Hebrew at an odd byte offset). An asset imported with a long CJK file name can never be renamed.
- **Fix.** Truncate on a code-point boundary, or `dump(-1, ' ', false, error_handler_t::replace)`. Add a long multi-byte test.

**H4. [High] `src/ui/main_window.cpp:808-836`. Verified code path; trigger timing possible.**
- **Defect.** `confirm_close(tab)` runs `QMessageBox::question`, a nested event loop, then `save_current(false, true)`. `save_current` saves `current_tab()` (`:747-751`), not `tab`. `close_tab(int index)` then calls `removeTab(index)` with the index captured before the modal.
- **Scenario.** While the prompt is up, a queued library `open_requested` or any `open_document` makes a new tab current. "Save" then saves the other document, and `close_recovery()` deletes the dirty tab's recovery data. Unsaved work is lost. If a tab is inserted before `index`, the wrong tab is removed.
- **Fix.** Save through `save(DocumentController&, …)` for the given tab. Hold a `QPointer<EditorTab>` and re-resolve the index with `tabs_->indexOf(tab)` after the modal.

**H5. [High] `src/ui/inspector.cpp:172, 443-460`. Verified.**
- **Defect.** `QSpinBox::editingFinished` fires on focus loss. `apply_note_fields` writes `velocity_->value()` to every selected note, and that field shows only the first note's velocity.
- **Scenario.** Select 50 notes with varied velocities, click into Velocity, click back into the roll. All 50 now have one velocity, as one undo entry.
- **Fix.** Apply only when the spin box value differs from the value shown at refresh (or when `isModified`). For mixed selections, show "mixed" and skip.

**H6. [High] `src/ui/piano_roll.cpp:1172-1180`, with `editor_tab.cpp:235-236` and `piano_roll.cpp:1280, 1285`. Verified.**
- **Defect.** The scroll bars call `set_origin` and `set_top_pitch`, which only call `update()`. Lanes re-sync only on `mapping_changed` or `document_changed`.
- **Scenario.** After a scroll-bar scroll, controller lanes still use the old `mapping_`. A velocity or CC stroke in a lane converts x to ticks with that stale mapping (`controller_lane.cpp:189`) and edits events the user is not looking at.
- **Fix.** Emit `mapping_changed()` from both setters, guarded against re-entry with `sync_scrollbars`.

**H7. [High] `src/jobs/jobs.cpp:77-80, 134-139`. Verified.**
- **Defect.** A finished task decrements `running`, notifies `idle`, releases the lock, then calls `dispatch()`, which reads `implementation_`. `~JobCoordinator` returns as soon as `wait_idle` sees `running == 0`. `~unique_ptr` then nulls the pointer before `~Implementation` joins the pool.
- **Scenario.** Destroying a coordinator right after its last job (every `jobs_tests.cpp` case) races a null dereference on a pool thread.
- **Fix.** Call `implementation_->pool.wait()` in the destructor before members are destroyed, or re-dispatch inside the same critical section without touching `this` after the notify.

**H8. [High] `src/midi/codec.cpp:225, 385-387` (libremidi `reader.cpp:571` reserve). Verified ordering; memory figures possible.**
- **Defect.** `reader.parse` materializes everything first, then each track is converted: one `EventId::generate()` and one 72-byte `Event` per event, plus one `Diagnostic` per malformed event. Only after that is `total_events > max_events` checked. There is no `try` around the conversion.
- **Scenario.** A crafted file within the default `max_bytes = 256 MiB` (`include/mwb/midi/*.hpp:22`), holding one MTrk of running-status events (about 85M events), exhausts memory. `std::bad_alloc` escapes a `Result` API, and in-process import (library, open) kills the app.
- **Fix.** Pre-scan MTrk lengths against an event budget before `reader.parse`. Check `total_events + source.size()` before converting each track. Cap diagnostics per code. Catch `bad_alloc` at the adapter boundary.

**H9. [High] `src/worker/*` and `src/jobs/*` have no production callers. Verified by grep.**
- **Defect.** Nothing outside these modules and their tests references `WorkerClient`, `JobCoordinator`, `mwb/worker` or `mwb/jobs`. SMF/RMID bytes are parsed in the GUI process: `src/app/document_io.cpp:109`, `src/library/library.cpp:625, 1618, 2292`.
- **Scenario.** Together with H8 and the core memory findings (M-core-4), one malformed file in an imported folder crashes the desktop app.
- `IMPLEMENTATION_STATUS.md` honestly lists "worker isolation (available, not yet used by library import)". The contract of section 28.2 is still unmet.
- **Fix.** Route app and library import through `WorkerClient` under `JobCoordinator` (after H7).

**H10. [High] `src/music/transform_time.cpp:215, 228-230, 286, 309, 427, 576, 583`; `transform_generate.cpp:102, 388, 401`; parameters read at `transform_json.cpp:166, 199, 274`. Verified.**
- **Defect.** `earliest + ticks`, `after.end += ticks`, `copy * parameters.pitch_step`, `copy * velocity_step`, `group_start + spread` and the groove offsets are computed on unbounded values from recipe JSON.
- **Scenario.** `mwb-cli transform --recipe` with `"ticks": 9223372036854775807` (move) or `"pitch_step": 2000000000` (repeat) causes signed overflow, which is UB.
- **Fix.** Bound Tick parameters in each tool's validation (for example |ticks| ≤ 2^40, |step| ≤ 127) and use `checked_add`/`checked_mul` from `core/checked.hpp`, returning `limit_exceeded`. Add out-of-range parameter tests.

**H11. [High, hot path] `src/core/note_edit.cpp:57-102` with `src/core/change_set.cpp:400-405, 486-505`. Verified.**
- **Defect.** For every updated note, `find_event` (a linear scan) runs twice. `ChangeBuilder::replace` then does `erase` plus `insert` in the middle of the working vector when the tick changes.
- **Scenario.** Dragging a 50k-note selection in a 100k-event track costs about 10^10 element operations on the GUI thread. The repository's own `docs/testing/bench-e100k.json` reports 78 ms to prepare a 3,125-note move. That measured number grows with selection × track size.
- **Fix.** Build an `EventId → index` map once per touched track. Apply all updates to copies, then do one `set_events` per track (stable sort and renumber), as `delete_notes` already does.

**H12. [High, hot path] `src/core/note_edit.cpp:164-170`; `src/core/change_set.cpp:461-472, 507-514`. Verified.**
- **Defect.** `duplicate_notes` and `insert_notes` call `add_note` per note, which means two mid-vector `insert`s each.
- **Scenario.** Paste or duplicate of 20k notes into a 100k-event track costs about 4·10^9 moves on the GUI thread (`main_window.cpp` duplicate and `piano_roll` paste paths).
- **Fix.** Collect new events, append them, then sort or merge once per track. Also return `not_found` when `destination` does not exist (today `ChangeBuilder::insert` returns a nil id silently), and mark `insert`/`remove`/`replace` `[[nodiscard]]`.

**H13. [High, hot path] `src/document/recovery.cpp:213, 220-235`; connected on the GUI thread at `src/ui/document_controller.cpp:36` via `connect_recovery`. Verified.**
- **Defect.** `record()` computes `semantic_hash(result_document)`, a full SHA-256 over every event, after **every** edit, undo and redo. Every 100 commands, 10 MiB or 60 s, it also runs `native_to_bytes(result_document)`, which builds a whole SQLite image and runs an integrity check, synchronously.
- **Scenario.** `IMPLEMENTATION_STATUS.md` records native serialization of E100K at 1,294 ms. So editing a 100k-event document freezes the GUI for over a second at each checkpoint, and per-edit hashing adds an O(N) cost to every keystroke edit (not measured).
- **Fix.** Derive the base hash incrementally, or from history state identity. Take an immutable snapshot and serialize checkpoints on a job thread. Move `flush()` off the GUI thread (see H18).

**H14. [High, hot path] `src/ui/main_window.cpp:641-644`; `main_window.cpp:1080`; `src/ui/playback_service.cpp:197-206`. Verified.**
- **Defect.** Every `document_changed` of the transport owner calls `publish_playback()`, then `publish_document`, which runs `std::make_shared<const DocumentData>(document)`, a full deep copy. When the transport is idle, the active tab is the owner (`main_window.cpp` `current_tab_changed`), so this happens on every edit even when nothing plays.
- **Fix.** Publish only while playing or armed, debounced. Better, have `WorkingSession` keep a copy-on-write `shared_ptr<const DocumentData>` so publishing is a pointer copy.

**H15. [High, hot path] `src/ui/event_list.cpp:103-104, 108-130`. Verified.**
- **Defect.** `EventTableModel::rebuild` (`beginResetModel`, a `stable_sort` over all events, and a map rebuild) is connected to both `document_changed` **and** `view_state_changed`. The panel is created on every tab switch, even when its dock is hidden (`main_window.cpp` `current_tab_changed`).
- **Scenario.** Ruler scrubbing fires `view_state_changed` on every mouse move, so each move costs O(N log N) plus a proxy reset at 100k events. Every edit also loses the table's selection and scroll position. The same signals rebuild `TrackListWidget` (`editor_tab.cpp:62-63`) and `InspectorPanel::refresh` (`inspector.cpp:326`).
- **Fix.** Drop the `view_state_changed` connection. Apply row-level updates from the change set's touched events. Create the panel only when its dock is visible.

**H16. [High, hot path] `src/ui/controller_lane.cpp:143-160`. Verified.**
- **Defect.** CC, pitch-bend and pressure lanes iterate all `track.events` on every paint and add every match to the polyline, including events off screen. Paint runs on every pan or wheel step (`mapping_changed` → `sync_lanes`) and on every edit.
- **Fix.** `lower_bound` to the first visible tick, carry the previous value, and stop past the last visible tick. Cache per-lane event indices per document generation.

**H17. [High] `src/ui/library_panel.cpp:80, 342-347` (searches at `:385`, `:434`). Verified.**
- **Defect.** All library work goes through one serial `task_thread()`, and `import_folder` posts `import_paths` followed by `analyze` as a single task.
- **Scenario.** During a large import, every search, open and metadata edit waits minutes, although the panel says browsing stays available (`library_panel.hpp`, and the import status text). Obsolete searches are still executed; generations are dropped only on delivery.
- **Fix.** Use a separate read executor using the WAL reader pool, and a writer/import executor. Check the generation before running a queued search.

**H18. [High] `src/ui/main_window.cpp:188-189, 857-866`; `src/ui/document_controller.cpp:42-55`. Verified call path; stall length not measured.**
- **Defect.** A 2 s GUI timer calls `RecoveryJournal::flush()` for every tab: a SQLite commit with `synchronous=FULL`. These also run on the GUI thread:
  - open (`app::open_document`);
  - native save, which `IMPLEMENTATION_STATUS.md` says takes 1.29 s for E100K;
  - MIDI export;
  - the recovery list and recover;
  - `std::filesystem::exists` in `LibraryPanel::choose_library`.
- This contradicts spec 19.4: "the GUI thread never waits for SQLite, the filesystem…".
- **Fix.** Move journal flush, save, export and open onto a document task worker. Deliver results by queued call with a generation check.

**H19. [High, hot path] `src/search/motif_match.cpp:565-586`. Verified code path; cost possible (estimated, not measured).**
- **Defect.** Approximate search reads every 3-gram of both families at up to `posting_limit` = 200,000 postings each. Each posting is inserted into `std::map<int64, std::map<int64, size_t>>`, and the `candidate_streams` budget is applied only afterwards. Cancellation is checked between grams, not inside the posting loop.
- **Scenario.** A 16-note query with common grams means up to about 5.6M node-allocating inserts per search. That is an estimated 0.5–2 s and hundreds of MB per keystroke-driven search.
- **Fix.** Order grams by `count_postings` and skip or cap ubiquitous grams. Accumulate votes in a sorted flat vector (postings are already ordered by stream and ordinal). Check `stop` inside the loop.

### Medium

**core / midi**
- `src/core/time.cpp:11-18`: `Rational::make` negates `INT64_MIN` (UB), reachable from native `settings_json` `"snap":"1/-9223372036854775808"`. Reject `INT64_MIN`.
- `src/core/tempo_map.cpp:34-44, 108-122, 147, 168`: `round_fraction` narrows `__int128` to `int64` without a range check. Huge tick counts give negative frames. Saturate, or reject over-long sequences at import.
- `src/core/notes.cpp:49, 65`: one `std::deque` per (port, channel, pitch) key, each with a 4 KB libc++ block, up to 524,288 keys, so about 2 GB on crafted input. Use flat per-key FIFOs.
- `src/core/ids.cpp:24-34`: `Uuid::generate` makes four `std::random_device` calls per id, once per imported event (8M OS RNG calls for a 2M-event file). The hand-rolled v4 generation is not recorded as a rule-1 exception (`IMPLEMENTATION_STATUS.md` says "none"). Use stduuid's generator over a seeded CSPRNG, or record the exception.
- `deps/libremidi/.../writer.cpp:204`, reached from `src/midi/export.cpp:402`: the writer drops explicit end-of-track events and appends a new one only if the last three bytes are not `FF 2F xx`. A track ending in such payload bytes gets no EOT. Add patch `0003` that always appends EOT.

**music**
- `src/music/analysis.cpp:376-379`: `analyze_sequence` is O(tracks × notes) on every library import. Bucket notes by track once.
- `src/music/transform_time.cpp:618-620`: `split_notes` is O(notes × points). Use `lower_bound`.
- `src/music/progression.cpp:243-275, 393, 105, 268`: the voice-leading DP is O(steps·K²), with `candidate_limit` from JSON capped only at 200,000, and the weighted costs overflow. Cap K and the weights.
- `src/music/transform_time.cpp:561-590`; `transform_generate.cpp:201-203`: generators have no total-output budget (`repeat` can create 1e8 notes). Add a global created-notes limit.
- `src/music/chord.cpp:408-421`: `analyze_chords` is O(n²). It is latent: there is no production caller.

**document / jobs / worker**
- `src/document/recovery.cpp:183-199`: after one failed `record()`, `last_generation_` never advances, so every later record fails "not contiguous" and crash protection silently stops. Resync, or force a checkpoint.
- `src/document/recovery.cpp:443-481`: replay never runs `validate_document`. A checksummed but invalid journal (for example `ppq=0`, accepted at `command_codec.cpp:437-447`) yields an invalid recovered document.
- `src/document/session.cpp:26`, `recovery.cpp:196`: each edit copies the `ChangeSet` twice (history by value, then the envelope) on the GUI thread.
- `src/jobs/jobs.cpp:94-95, 155-158`: the bounded `all` history evicts running jobs, so `cancel_all` misses them and the destructor hangs until they finish.
- `src/worker/protocol.cpp:116-119`; `worker_client.cpp:262, 298, 308-309, 316`; `worker_service.cpp:200, 213-215`: `json::value()` throws on wrong types from the peer. Exceptions cross IPC (rule 5), and the worker terminates instead of sending a failure frame.
- `src/worker/protocol.cpp:83, 127`; `worker_client.cpp:43, 88`: `from_msgpack` recursion has no depth limit (stack overflow from 1 MiB of `0x91`), the received deque is unbounded although commented "bounded", and `ChunkAssembler` reserves a peer-declared 512 MiB.
- `tests/fuzz/ipc_frames_fuzz.cpp:210-216`: every frame needs a valid XXH3, so the fuzzer cannot reach msgpack decoding or `ChunkAssembler`. Add a payload-level target.

**search**
- `src/search/browse_index.cpp:422-481, 284-295, 348-353`: `page()` re-evaluates, copies and sorts every row per call, and query tokens are recomputed per row. It is latent: `BrowseIndex` is not instantiated in production, which also means the section 19.4 in-memory index is not delivered.
- `src/search/browse_index.cpp:261-263`: `clear()` keeps string capacity, so the memory budget overcounts and falls back to SQL early.
- `src/search/browse_index.cpp:148` vs `sql_compiler.cpp:117-126`: `NOT key:"C minor"` with a NULL `key_mode` differs between SQL (NULL, so excluded) and the index (included). Emit `key_mode IS NOT NULL AND …`.
- `src/search/sql_compiler.cpp:513-515`: `INDEXED BY` forces a sort-order index walk, so a sparse filter scans 100k rows. Possible (planner reasoning). Add EXPLAIN tests for sparse pages.
- `src/search/motif_match.cpp:176-196`: the short-query scan checks the result limit and cancellation only per 256-stream batch.
- `src/search/motif_match.cpp:63-83, 372-403`; `motif_stream.cpp:331-375`: other waste:
  - the stream cache holds up to 4096 decoded streams;
  - the banded alignment allocates and iterates the full matrix;
  - `gram_hash` heap-allocates per gram;
  - `family_tokens` is rebuilt four times per stream.

**library**
- `src/library/library.cpp:1995-2090, 2097-2130`: `load_browse_row` prepares 6+ statements per asset (N+1). Production uses `browse_rows_for(ids)` per search page (`library_panel.cpp:397, 479`; `cli/library_commands.cpp:71`), so a 100-row page is about 700 prepares. The full-index build path has no production caller. Use set-based batch queries.
- `src/library/library.cpp:2292-2296, 2390`: a cancelled analysis is recorded as `status='failed'` and never retried.
- `src/library/library.cpp:1522-1533, 1758-1760`: objects published before a failed or conflicting transaction are never garbage-collected (GC walks only `object_file` rows).
- `src/library/storage.cpp:362-367`: an existing object is reused on a size match alone, and the hash-verified staging copy is discarded.
- `src/library/library.cpp:1569, 501-522`: `restore_revision` and `duplicate_asset` drop the `asset_override` key override.
- `include/mwb/library/library.hpp:336`; `library.cpp:1716-1728`: `list_trash` is documented as keyset paged but takes no cursor.
- `src/library/library.cpp:739-755, 1794`: folder iteration stops silently on error (recursive), or throws `filesystem_error` out of a `Result` API (non-recursive, and the GC staging scan).
- Statements re-prepared per row in loops: `library.cpp:1362-1414, 1633-1708, 2333-2383, 2614-2627`. `move_in_collection` rewrites every position per move, and `rebuild_fts` holds the writer for all assets in one transaction.
- `src/library/maintenance.cpp:334-343`: `restore_backup` opens an external catalog with no schema whitelist or `sqlite3_limit`.
- `src/library/library.cpp:1739`; `maintenance.cpp:189`: GC deletes files outside the writer lock, so it races backup and object-reusing imports. Possible (depends on the GC thread).

**playback / audio / platform** (render-path findings verified by the lead reviewer)
- `src/playback/scheduler.cpp:197-209`: in held-note chase mode, every seek and loop wrap scans `snapshot_->notes` from index 0 **on the render thread**. That is O(100k) for a loop near the end of a large song. Store notes crossing each checkpoint in the snapshot.
- `src/playback/scheduler.cpp:372-385` with `snapshot.cpp:493-509`: loops ≥ 1 frame are accepted, and each wrap in a block runs `release_owned`, `sustain_off` and `chase_to`. A 1-tick loop gives about 160 chases per 4096-frame block, while the preflight still says "allowed". Enforce a minimum loop length, or cap wraps per block.
- `src/platform/audio_engine.cpp:54-55`: external queue overflow drops events, including note-offs from stop or panic, so hardware notes stick. Possible (the consumer's lag was not traced). Reserve cleanup headroom, or flag the overflow so the output thread sends All Notes Off.
- `src/audio/offline_renderer.cpp:110-115`: the temp name is built from `filename().string()`, which narrows to the ANSI code page on Windows, so rendering to a Hebrew file name fails. Possible. Build the name as a `path`.

**ui**
- `src/ui/document_controller.cpp:150-177, 234-244`: every edit eagerly rebuilds `pair_notes`, `NoteIndex`, `TempoMap`, `MeterMap` and a selection map. With core `update_notes` that is two pairings per nudge.
- `src/ui/drum_grid.cpp:58-66, 71, 82, 157`: `rows()` scans every note into a `std::set` on each paint and click.
- `src/ui/piano_roll.cpp:1128-1171`: `select_same_pitch`, `select_same_pitch_class` and `select_same_channel` are O(N·S). Use bitmasks.
- `src/ui/piano_roll.cpp:94-99, 305-406`: each 30 Hz playhead tick repaints the whole roll. Use `update(QRect)` for the playhead strip and cache the density image.
- `src/ui/main_window.cpp:970-` (`transpose_selection_by_degree`): keeps a computed preview across `QMessageBox::question`. Meanwhile the 4 ms `poll_input` timer can commit recording edits, and the stale ChangeSet is then applied. Re-check `generation()` after the modal.
- `src/ui/main_window.cpp` (`close_tab` and `closeEvent`) with `recording_controller.cpp:72-74`: closing a recording tab commits the take after the save prompt and after recovery was closed. `closeEvent` never stops recording, so a take in progress at quit is lost.
- `src/ui/library_panel.cpp` (search and import posts) and `playback_service.cpp:95-104`: worker lambdas read `guard.data()` and then `postEvent` to that raw pointer. `~LibraryPanel` does not join, so the panel can be freed in between (at shutdown). Post to `qApp` and check the `QPointer` on the GUI thread.
- `src/ui/library_panel.cpp` (`run_search`): only the first page is ever fetched; `SearchPage::next` is ignored.
- `src/ui/clipboard.cpp:161-169`: paste arithmetic overflows on large untrusted ticks, and `mul_div_round(...).value_or(0)` silently moves notes.
- `src/ui/clipboard.cpp:36`: `dump()` throws on an invalid-UTF-8 track name during copy. Possible.
- `src/ui/event_list.cpp:337-338`: `if (selection.size() > 2000) break;` is inside the loop, so a large selection syncs only one row.

**app / cli**
- `src/cli/document_commands.cpp:281, 303`; `library_commands.cpp:45, 240, 270`: five throwing `nlohmann::json::parse` calls. Rule 1 allows only non-throwing parse.
- `src/cli/library_commands.cpp:109, 119`: `std::stoll` durations are unbounded and `onset += duration` overflows (UB).
- `src/app/recording.cpp:97, 45, 72`: two sources feed `raw_` out of time order, and `decode_take_chunk` rejects non-monotonic chunks, so the take's raw evidence cannot be decoded. Sort by `time_ns` in `finish()`.
- `src/app/recording.cpp:231`: `kept_pass = min(last_pass, stop_pass)` keeps the incomplete pass, which contradicts `IMPLEMENTATION_STATUS.md` item 9 ("last complete pass"). `app_tests.cpp:152-170` locks in the wrong behaviour.

**build / CI / tests / docs**
- `.github/workflows/ci.yml`: no fuzz-smoke job (spec section 36 requires one). clang-tidy and coverage are not run. The lint step "Verify the include graph tool runs" only runs `py_compile`.
- `tests/fuzz/CMakeLists.txt`: no fuzz target for the JSON recipe parsers (`parameters_from_json`, `selection_from_json`, which `mwb-cli transform --recipe` reaches), the clipboard JSON, or `decode_take_chunk`. Seed directories for `command_codec`, `ipc_frames`, `motif_stream` and `soundfont` are empty.
- `tools/check_include_graph.py:13, 27`: only `mwb/...` includes are matched, so `"../ui/x.hpp"` bypasses it. `ui` is absent from the graph when the GUI is off. There is no negative self-test.
- `tests/CMakeLists.txt:95-100`: the `include-graph` test silently disappears when Python is not found.
- Silent skips:
  - `tests/platform_tests.cpp:288-291` prints "SKIPPED" and returns, which counts as a pass.
  - `tests/worker_tests.cpp:114-117` and `:184` assert only `if (!parsed)`.
  - `tests/midi_tests.cpp:229-239` asserts only when import succeeds.
- Docs that do not match the code:
  - `deps/README.md:27` says miniz is used for ZIP backups, but nothing in `src/` uses miniz (still linked at `CMakeLists.txt:80`).
  - `docs/BUILDING.md:61-63` says "no device is touched", while `platform_tests.cpp:270-296` enumerates real WinMM and WASAPI devices.
  - `src/worker/worker.hpp:27` documents stall detection that is not implemented.

### Low (grouped)
- **core / midi**
  - `notes.cpp:154`: `NoteIndex::query` can miss a zero-length note at `window.begin`.
  - `change_set.cpp:85-189`: an insert with a duplicate identity is not rejected.
  - `change_set.cpp:168-188`: `bad_alloc` mid-commit leaves the document half-applied.
  - `change_set.cpp:337-362`: `memory_estimate` ignores take and region payloads.
  - `change_set.cpp:442`: order-key wrap near `UINT64_MAX`.
  - `tempo_map.cpp:141-165, 220-226`: linear segment scans per call.
  - `random.cpp:37`, `pitch.cpp:83`: overflow edges.
  - `codec.cpp:313-318`: the diagnostic says "preserved" but the event is dropped.
  - `hashing.cpp:42-44`: raw `new`/`delete`.
- **music**
  - `grid.cpp:80, 128, 167` and `transform_generate.cpp:31, 39`: `make_rational` failure silently falls back to `{0,1}`.
  - `rotate` overflow at `transform_time.cpp:478, 495`.
  - `render_progression`/`render_chord` add before the checked add.
  - `change_length` legato is quadratic per equal-onset run.
  - `set_sequence_scale`/`insert_progression` copy the whole `Sequence`.
  - `NoteEditor::open` builds locations for every track.
  - The humanize inversion guard uses one channel per group.
- **document / jobs / worker**
  - `command_codec.cpp:804`: CBOR depth is not limited.
  - `worker_client.cpp:182-194`: pipe handle leak, and an ignored `UpdateProcThreadAttribute`.
  - POSIX SIGPIPE.
  - Compaction deletes the older checkpoints the fallback needs.
  - Untrusted-connection hardening lacks `cell_size_check` and `mmap_size=0`.
  - `native_format.cpp:933-947`: TOCTOU between the header check and the SQLite open.
  - `file_system.cpp:111-116`: POSIX dir-fsync failure is reported after a successful rename.
  - `wait_idle` contradicts its docs while paused.
  - `session.cpp:56, 74`: `save()` deep-copies twice on the GUI thread.
- **search**
  - Unreachable `sequence_exists(builder, {})` fallback.
  - The FTS rank has no `id` tie-break.
  - A browse-index cursor with a mismatched type is accepted.
  - The token dictionary never shrinks.
  - `duration:1e308min` accepted as infinity.
  - The nesting-depth message miscounts.
  - The decoder never checks the profile version.
  - `InMemoryMotifIndex::add` is quadratic.
  - Rhythm-mode retrieval vs tolerance.
  - NaN weights break the `stable_sort` ordering.
- **library**
  - `verify` opens read-write, so the WAL checkpoint modifies files.
  - `maintenance.cpp:356`: ANSI `string()` path.
  - The reader pool waits forever when `read_connections == 0`.
  - A second-source failure discards the first source's report.
  - `changes_since` uses two implicit transactions.
  - The outbox is never pruned, and no-op changes still bump the generation.
  - Pre-1970 FILETIME wrap.
  - The public header exposes SQLiteCpp.
  - Motif search installs no progress interrupt.
- **playback / audio / platform**
  - `BlockingReaderWriterQueue::try_enqueue` on the render thread can call `ReleaseSemaphore` (a syscall, not a lock) when the MIDI-output thread waits. Consider a non-blocking queue polled on a timer.
  - `remove_note` is O(owned notes).
  - CC123 is O(V²) in the built-in synth.
  - Publish before `open()` skips the sample-rate check.
  - `close()` does not drain the queues.
  - Stale timbre overrides.
  - SysEx `enqueue` allocates under `noexcept`.
  - `offline_renderer.cpp:248`: size multiply can wrap.
  - `file_watcher.cpp:28-35`: case-sensitive path compare on Windows.
  - Raw MIDI input bypasses `ignore_timing`/`ignore_sensing`.
  - The SF2 fuzz target covers only `load_memory`.
- **ui**
  - Paste and double-click find created notes by search instead of id.
  - `status_message` is emitted from `paintEvent`.
  - `zoom_history_` is unbounded.
  - `__int128` → `int` overflow when scrolled past the end.
  - Errors are dropped silently in draw, erase, drum toggle and several `main_window` commands.
  - `std::ignore = engine_->post(...)` without a comment (`playback_service.cpp:166, 175, 182, 204, 211`).
  - `editor_math.cpp:217-243`: `value_or(0)` correction loops.
  - Destruction order of `playback_` vs child tabs.
  - Highlight/fold toggles create undo entries (events unchanged; check against spec 9.1).
  - `CommandPalette` triggers the action inside its own `exec()`.
  - `LibraryPanel` resets the model after every metadata edit.
- **app / cli**
  - `environment.cpp:77`: ANSI log path.
  - `start_logging` reports `ok` on its second call.
  - `capabilities` reports backends from environment variables.
  - Overwrite TOCTOU in `document_io.cpp:62-86` and the CLI writers.
  - `encode_take_chunk` does not bound `size ≤ 3`.
  - A cancelled import reports `status: ok`.
  - `--warnings-as-errors` leaves `errors` empty.
  - A `make_recipe` error is dropped.
  - Step `tie`/`back` re-pair the whole track per key press.
- **build**
  - `update_deps.py:160-163`: `rmtree` + `copytree` is not atomic, despite its docstring.
  - `wait_until` busy-spins with `yield`.
  - Fuzz targets do not get the owned warning flags.
  - License files are missing from `CONFIGURE_DEPENDS`.
  - The build timestamp prevents reproducible builds.

### Nit (grouped)
- **Missing `///` comments:**
  - parameter structs in `transform.hpp`, `analysis.hpp`, `fingerprint.hpp`, `motif.hpp`, `voice.hpp`;
  - `library.hpp:39-41, 205-215, 279-313`;
  - `cli/commands.hpp:51-72`;
  - private helpers in search, worker and the scheduler;
  - `NullAudioOutput`, `MidiInputOptions`/`MidiOutputOptions`.
- **Missing includes:** `recording_controller.hpp` (`<algorithm>`, `<vector>`), `inspector.hpp` (`<memory>`), `midiworkbench_main.cpp` (`<algorithm>`), `main_window.cpp` (`<cstdio>`), and `query_parser_fuzz.cpp` (`<tuple>`, `<cstdint>`).
- **Compiler extensions:** `__int128` in `checked.hpp`, `motif_match.cpp`, `piano_roll.cpp` and `editor_math.cpp`. Accepted for Clang-only builds; note it if MSVC is ever considered.
- **Unused links:** `jobs` links `concurrentqueue`, `search` links `sqlitecpp`, and `mwb_app` links `miniz`, none of which they use. `deps/CMakeLists.txt` builds midifile even with tests off.
- **Macro:** `MWB_TRY_QUERY` is defined twice in `query_parser.cpp` (`:527`, `:623`) and never `#undef`'d.
- **Unused member:** `library_panel.hpp` `workers_`.

---

## Owner-rule compliance matrix (`AGENTS.md`)

| # | Rule | Verdict | Evidence |
|---|---|---|---|
| 1 | Dependencies first | **Partial** | **Compliant uses:**<br>- libremidi reader/writer for SMF (`codec.cpp:224`, `export.cpp:375`)<br>- PEGTL grammar (`query_parser.cpp:9-117`)<br>- picosha2/xxHash (`hashing.cpp`), pcg (`random.cpp:3`), utf8proc (`text.cpp:84`), rapidfuzz (`command_palette.cpp:52`)<br>- BS::thread_pool (`jobs.cpp:64`), moodycamel (`audio_engine.cpp:28-30`), TinySoundFont, dr_wav, efsw<br><br>**Deviations:**<br>- Hand-rolled UUID v4 generation (`core/ids.cpp:24-34`), while `IMPLEMENTATION_STATUS.md` records "Recorded exceptions … none".<br>- Hand-rolled serial task queues (`library_panel.cpp:33-80`, `playback_service.cpp:29-87`).<br>- Five throwing `json::parse` calls in the CLI.<br>- miniz is linked but unused, while its documented ZIP backup does not exist. |
| 2 | Patches only | **Compliant** | `python deps/update_deps.py --check` returned `checked 24 dependencies: OK` (run by this review). libremidi changes are in `patches/libremidi/0001*`, `0002*` and listed in the manifest. CI runs `--check` (`ci.yml:18, 45`). |
| 3 | Names and `///` docs | **Partial** | Names are clear throughout and public APIs are mostly documented. Many parameter structs, private helpers and some public headers lack `///` (see Nits). |
| 4 | Layout and include graph | **Partial** | Headers live in `include/mwb/<module>/` and `src/` holds only `.cpp`. The `include-graph` CTest passed in this review's run, and no reviewer found a forbidden include. But the checker misses relative includes, drops `ui` when the GUI is off, and has no negative test (`tools/check_include_graph.py:13, 27`). |
| 5 | Errors | **Partial** | `Result`/`[[nodiscard]]` everywhere, no `(void)` casts in `src/` or `include/`, and `std::ignore` mostly commented. Gaps:<br>- Exceptions cross worker IPC (`worker_client.cpp:262, 298, 308`).<br>- Exceptions escape `Result` APIs (`import_midi` `bad_alloc`; `filesystem_error` at `library.cpp:753, 1794`) and a Qt slot (`clipboard.cpp:52`).<br>- `ChangeBuilder::insert/remove/replace` lack `[[nodiscard]]`, and their results are dropped (`note_edit.cpp:75, 93, 100`).<br>- Uncommented `std::ignore` at `playback_service.cpp:166-211`. |
| 6 | Tests | **Partial** | doctest `TEST_SUITE`s with a `--mwb-require-tests` guard; no fixed sleeps anywhere in `tests/` (grep for `sleep_for`, `Sleep(`, `msleep`, `qWait`). Gaps:<br>- Silent skips at `platform_tests.cpp:288-291`, `worker_tests.cpp:114-117, 184` and `midi_tests.cpp:229-239`.<br>- Whole components untested: `note_edit` in core, `EventTableModel`, `ControllerLane`, `DrumGridView`, `InspectorPanel`, `LibraryPanel`, `RecordingController`, `JobCoordinator` shutdown and cancel.<br>- One test locks in behaviour that contradicts the status doc (`app_tests.cpp:152-170`). |
| 7 | Warnings | **Compliant** (debug preset with `-DMWB_WERROR=ON`) | This review's build in `build/review`: 205/205 steps, 0 warnings. `-Werror` is wired to the `release`, `sanitize` and `ci` presets (`CMakePresets.json:41, 50`). Fuzz targets are not compiled with the owned warning set (`tests/fuzz/CMakeLists.txt:115-122`). |
| 8 | Platform isolation | **Compliant** | `_WIN32` with a POSIX branch in `audio_output_miniaudio.cpp`, `midi_backend.cpp`, `storage.cpp`, `file_system.cpp`, `worker_client.cpp`, `environment.cpp` and the main programs. Public headers hold only one WinMM constant (`midi_backend.hpp:45`, data only). |
| 9 | Real-time paths | **Partial** | The full render call graph from `MiniaudioOutput::data_callback` through `AudioEngine::render`, `Scheduler`, `BlockRenderer` and the synths was traced. It has no allocation, lock, logging, Qt call, file access or free; snapshot retirement happens on the owner thread (`handoff.hpp`, `audio_engine.cpp:166`). The MIDI input callback only enqueues to a preallocated queue. Gaps:<br>- Unbounded work per block: held-note chase over all notes (`scheduler.cpp:197-209`) and loop wraps (`:372-385`).<br>- A semaphore syscall in `BlockingReaderWriterQueue::try_enqueue`. |
| 10 | Docs move with code; no unobserved results | **Partial** | `IMPLEMENTATION_STATUS.md` (created during this review) cites runs, attaches logs and benches, and marks "not verified" honestly. But:<br>- It says loop recording keeps "the last complete pass" (contradicted by `recording.cpp:231`) and "Recorded exceptions … none" (contradicted by `ids.cpp:24-34`).<br>- `deps/README.md:27` claims miniz ZIP backups.<br>- `BUILDING.md:61-63` says no device is touched.<br>- `worker.hpp:27` promises stall detection.<br>- `library.hpp:336` says `list_trash` is "keyset paged".<br>- `library_panel.hpp` says browsing works during import. |
| 11 | Commits only with owner OK | **Not assessed** | This review ran no git commands, as instructed. |
| 12 | Product invariants | **Partial** | Compliant parts:<br>- Change sets commit atomically (`change_set.cpp`, `history.cpp:115-150`).<br>- Imported bytes are preserved and verified (`codec.cpp:587`, `export.cpp:328-333`, `native_format.cpp:148, 778`).<br>- Import copies bytes into the object store.<br>- No network symbols in `src/`; external MIDI only on explicit routes.<br>- Scale highlight and fold never change events.<br><br>Gaps:<br>- Highlight and fold toggles create undo entries and mark the document dirty (`inspector.cpp:460-478`).<br>- A take recorded while closing a tab is committed after the save prompt and lost.<br>- A stale ChangeSet can be applied after a modal (`transpose_selection_by_degree`).<br>- The velocity focus-out edit (H5) is an unintended document change. |

---

## Cross-cutting themes

1. **The GUI thread does whole-document work for every edit** (H11–H18, plus the core and ui Mediums). One user edit costs at least:
   - two full pairings;
   - one full SHA-256;
   - one deep copy;
   - one event-table sort and reset;
   - several O(N) panel refreshes;
   - in core, O(k·n) vector surgery.

   The architecture in sections 19.4 and 20.3 (immutable snapshots, a document task worker, incremental updates from the change set) is specified but not yet followed. One shared fix covers most of this: `WorkingSession` publishes a `shared_ptr<const DocumentData>` together with the applied `ChangeSet`, and every consumer updates incrementally or lazily.
2. **Untrusted data reaches throwing or unbounded code.**
   - nlohmann `at()`, `value()` and strict `dump()` appear in the clipboard, CLI, library outbox and worker client.
   - SMF limits are checked after materialization.
   - Recipe and clipboard ticks reach unchecked arithmetic.
   - msgpack and CBOR decoding have no depth limits.
   - The worker isolation that would contain all of this is not wired in.

   Rule: every external value is range-checked at the boundary. Every `dump()` uses `error_handler_t::replace` (or names are validated as UTF-8 at load). The JSON readers use only `find()` plus type checks.
3. **Quadratic or N+1 paths that pass at 1k and fail at 100k:**
   - `find_event` per note;
   - per-note vector insert;
   - per-row statement prepare;
   - analysis per track;
   - `split_notes`;
   - approximate-search voting;
   - per-paint full-track scans.

   Only catalog paging and E100K pairing have recorded benchmarks. The 78 ms for a 3k-note move in `bench-e100k.json` is the early signal.
4. **Built but not integrated.** The worker, the job coordinator and the in-memory `BrowseIndex` are implemented and tested in isolation, but nothing in the product uses them. Their tests give coverage of code that does not run in the product, while the in-process paths they were meant to replace go untested at scale.
5. **Wide-path handling on Windows.** `path.string()` narrows to the ANSI code page in the offline renderer, the log file, the restore safety backup and the tests. This contradicts the project's own Hebrew/emoji path tests for native save.
6. **Verification gaps in CI.** Fuzzing is never run, clang-tidy and coverage are never run, the include-graph test can vanish, and three tests skip silently. `IMPLEMENTATION_STATUS.md` says CI has never executed.

---

## Suggested order of work

1. **Crash and data-loss fixes (small, independent):**
   - H1, H2, H3: non-throwing JSON access and UTF-8-safe `dump`/truncation, plus tests with malformed and multi-byte input.
   - H4: save the closed tab; `QPointer` and an index re-check after the modal.
   - H5: apply only changed inspector fields.
   - H6: emit `mapping_changed` on scroll.
   - H7: join the pool in `~JobCoordinator`.
2. **Untrusted-input bounds:**
   - H8: pre-count events and cap diagnostics.
   - H10: bound transform parameters and use checked arithmetic.
   - The core Mediums: `Rational::make`, `tempo_map` narrowing, per-key deques.
   - Clipboard tick bounds and CLI `--notes` bounds.
   - Worker JSON type checks and msgpack/CBOR depth limits.
   - Add fuzz targets for recipes, clipboard and take chunks, and a fuzz-smoke job in CI.
3. **GUI-thread edit pipeline (theme 1):**
   - H11 and H12: an id→index map and one `set_events` per track.
   - H13: incremental hash and off-thread checkpoint.
   - H14: copy-on-write document snapshots.
   - H15 and H16: incremental event table, visible-range lane paint.
   - Then the `DocumentController` cache rebuilds, drum-grid rows and select-same bitmasks.
   - Add E100K benchmarks for drag, duplicate and paste, and assert a budget.
4. **Off-GUI I/O and isolation:**
   - H18: journal flush, save, open and export on a document task worker.
   - H17: separate library read and import executors.
   - H9: route import through `WorkerClient` and `JobCoordinator`.
5. **Search and library scale:**
   - H19: ordered grams with caps and flat vote accumulation.
   - Set-based `browse_rows_for`; prepare statements once per loop.
   - Record a cancelled analysis as cancelled, not failed.
   - Orphan-object GC; hash-verify objects before reuse.
   - EXPLAIN tests for sparse filters and motif queries.
6. **Real-time budget:**
   - Cap loop wraps per block and enforce a minimum loop length.
   - A per-checkpoint held-note index.
   - A cleanup reserve in the external queue.
   - A test that counts `operator new` around `AudioEngine::render`.
7. **Docs and test hygiene:**
   - Correct `IMPLEMENTATION_STATUS.md` (recording pass; the UUID exception), `deps/README.md` (miniz) and `BUILDING.md`.
   - Replace the silent skips with doctest `skip()` or required failures.
   - Harden `check_include_graph.py`.
   - Add the missing `///` comments.

The next round should re-check every finding above and record its status, as section 34.7 requires.
