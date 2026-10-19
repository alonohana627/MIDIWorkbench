# Implementation status

Specification: `MIDI_Workbench_Full_Specification.md` v1.1. This file records what exists, what
was verified (with the commands that ran), deviations, exceptions and what is left. Each claim
cites a run or says "not verified".

Last update: 2026-10-19. No git repository has been created and nothing has been committed
(commits need the owner's OK, section 35.2).

## Environment of the verified runs

Windows 11 Pro build 26200, Intel Core Ultra 7 270K Plus, 64 GiB RAM. MSYS2 CLANG64: clang 23.1.3,
CMake 4.4.4, Ninja 1.13.2, Qt 6.11.2, libc++ (`_LIBCPP_VERSION` checked at configure).

## Last verified commands

| Command | Result |
|---|---|
| `python deps/update_deps.py --check` | `checked 24 dependencies: OK` |
| `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` | `100% tests passed out of 56` (after the review fixes and the UI redesign) |
| `cmake --preset release && cmake --build --preset release && ctest --preset release` (`-Werror`) | build clean; `100% tests passed out of 56` (58 in release: adds the E100K perf test) |
| `cmake --preset sanitize`, build, `ctest --preset sanitize` (ASan + UBSan) | `100% tests passed out of 56` after the review fixes and the UI redesign (log: `docs/testing/ctest-sanitize.log`) |
| `cmake --preset fuzz`, build, each `mwb_fuzz_*` for 30 s | 8 targets ran; `query_parser` found a stack-use-after-scope (fixed; the input is a regression seed); after the fix 60 s clean |
| `cmake --install build/release --prefix stage`; `stage\mwb-cli.exe capabilities --json` and `stage\MIDIWorkbench.exe --exit-when-ready` with `PATH=C:\Windows\System32`, offscreen, null backends | both exit 0 |
| `cpack -G ZIP` in `build/release` | `MIDIWorkbench-1.0.0-windows-x64.zip` (91.9 MB) |
| `mwb-cli bench --rows 100000` (release) | all hot queries well under the 150 ms target; no plan problems (`docs/testing/bench-catalog-100k.json`) |
| `mwb_bench_editor` (release, E100K: 32 tracks, 212,563 events) | see Performance (`docs/testing/bench-e100k.json`) |
| `MIDIWorkbench.exe --theme dark|light|high_contrast --screenshot … --exit-when-ready docs/images/demo.mid` | `docs/images/main-window-{dark,light,high-contrast}.png` written, exit 0 |

CTest entries (debug): core (time, notes, changes, core), midi (import, export), music (scales,
chords, progression, transform, analysis, fingerprint, voice), document (native, history,
recovery), search (query, motif), library (catalog, import, revisions, backup, plans), playback
(playback, scheduler, transport), audio (audio, render), platform, jobs, worker (ipc, worker),
app (recording, step_recording), cli, ui (editor_math, controller, piano_roll, keyboard,
clipboard), include-graph, and one `<area>.unnamed` entry per executable.

## Phase status (section 35)

| Phase | State |
|---|---|
| P0 toolchain proof | Done except the clean-VM test and CI runs (no repository/runner yet). Qt window, C++23 features, libc++, SQLite STRICT+FTS5, WASAPI and WinMM enumeration, SF2 load, deployment with System32-only PATH, sanitizers and libFuzzer probed (see notes) |
| P1 MIDI foundation | Done: IDs/time, SMF codec adapter, CLI inspect/validate, editor, built-in synth, playback, export |
| P2 serious editor | Done with gaps listed below |
| P3 input and devices | Mostly done: physical MIDI input, step and live recording, takes, WinMM output routing, state chase, device loss. Gaps below |
| P4 composition tools | Done: scales, chords, progressions, all section 10 tools, drum grid, SF2 (offline render), WAV render |
| P5 managed library | Done with gaps: object store, catalog, import, tags/collections/revisions/trash, text search, backup/verify/repair |
| P6 musical retrieval | Done with gaps: analysis, fingerprints, exact and approximate motif search, worker isolation for file open and library import |
| P7 release hardening | Partial: packaging ZIP, docs, diagnostics, fuzzing, sanitizers, benchmarks. Clean-VM, accessibility audit, MSI, signing not done |

## Requirements (section 3)

| ID | State | Evidence |
|---|---|---|
| WB-001 create/open/save/duplicate/recover | Done | document.native/history/recovery tests; GUI recovery prompt; ui.controller |
| WB-002 import SMF 0/1/2, PPQ/SMPTE | Done | midi.import (all SMPTE codes, PPQ 1..32767, RMID, unknown chunks, truncation), fuzz smf_import |
| WB-003 deterministic export | Done (Ableton/DAW matrix not run) | midi.export, midifile oracle test |
| WB-004 piano roll and navigation | Done (see gaps) | ui.piano_roll, ui.editor_math |
| WB-005 on-screen and computer keyboard | Done | ui.keyboard |
| WB-006 physical keyboard and recording | Done, not hardware-verified (this machine has no MIDI input) | app.recording, app.step_recording |
| WB-007 scale highlight/fold/pitch tools | Done | music.scales (golden 5), fold rows in the piano roll |
| WB-008 chords, arpeggio, rhythm, variation | Done | music.chords, music.progression, music.transform |
| WB-009 velocity, release velocity, CC, bend, pressure | Done | controller lanes; music velocity tools |
| WB-010 playback, seek, loop, note cleanup | Done (hardware endurance not run) | playback.scheduler stress (5,000 transitions), golden 8, zero loop drift |
| WB-011 native format and immutable revisions | Done | document.native, library.revisions |
| WB-012 incremental large-library import | Done (L100K real-file import not measured; only synthetic catalog rows) | library.import, bench |
| WB-013 text search, filters, saved searches | Done (smart collections store the AST; no saved-search UI). The library panel has text search, melody search ("hum" a phrase from the selection), tags, musical role, trash and drag-out of files | search.query, library parity tests |
| WB-014 melody/interval/rhythm/similar search | Done (no labeled retrieval corpus yet) | search.motif, cli `library melody`, GUI Library menu |
| WB-015 duplicate detection | Partial: fingerprints and byte-identical dedup exist; no duplicate-review/merge UI | music.fingerprint, library.import |
| WB-016 external MIDI and DAW handoff | Done for routing, panic, drag-out and drop-in; DAW matrix not tested | playback_service router, drag_export |
| WB-017 deterministic WAV render | Done (RF64 not possible, see deviations) | audio.render |
| WB-018 responsive, accessible Qt UI | Partial: accessible names on custom widgets, keyboard-operable event list, shape markers; no screen-reader or DPI audit | not verified |
| WB-019 reproducible toolchain and packaging | Partial: presets, vendoring, ZIP, install smoke test; clean VM and MSI not done | install smoke test above |
| WB-020 CLI and diagnostics | Done; also a shortcut editor and a command palette in the GUI | cli tests; Help > Diagnostics |
| WB-021 backup, restore, migration, trash, repair | Done (ZIP backups not implemented) | library.backup, document migration tests |
| WB-022 offline | Done by construction: no network code; not verified on a network-disabled install | not verified |

## Deviations and decisions (owner approval needed where marked)

1. **libremidi patches** (recorded in `patches/libremidi/`): SMPTE divisions, unknown chunks,
   header extensions, F7 marker, VLQ length limit, lenient known-meta lengths (reader); explicit
   format, raw division and end-of-track delta (writer). libremidi's `client.cpp` is not compiled
   (does not build with this libc++; not needed).
2. **signalsmith-dsp** is exposed only via `include/` because its root `windows.h` shadows the SDK.
3. **libFuzzer**: the CLANG64 driver rejects `-fsanitize=fuzzer` for windows-gnu; the `fuzz`
   preset uses ASan plus libFuzzer coverage flags and links `libclang_rt.fuzzer` explicitly.
4. **RF64**: miniaudio's encoder writes only RIFF; renders over 4 GiB are refused with a clear
   error. Needs a miniaudio patch or standalone dr_wav (owner decision).
5. **Take operations**: `TakeInsert`/`TakeRemove` were added to core change sets (and the
   recovery codec) so a recorded take is one undo entry with its raw evidence.
6. **Note link identity**: a native `note_link.id` equals its note-on event id (deterministic
   projection, section 16.4 "validated projections").
7. **Underflows**: miniaudio exposes no WASAPI glitch counter; "deadline misses" are reported.
8. **Audition** uses MIDI channel 16 of the built-in synth as the private audition channel instead
   of a second synth instance.
9. **Loop recording** (recorder only; the GUI does not expose it yet): every pass stays in the
   raw take evidence, and the composed result keeps the last pass recorded, which is the pass in
   progress when recording stopped even if it is incomplete (`src/app/recording.cpp:231`).
10. **Real-time take mapping** is arrival-estimated (steady clock relative to the transport start),
    plus the configured latency compensation.
11. **Query errors** exit with code 1 (invalid argument) in the CLI.
12. Catalog index tuning: 10 indexes added beyond the 23.2 DDL (allowed by section 23).

Recorded exceptions to section 4.4 (writing code a library could provide): none. Identities
use stduuid's generator over `std::random_device` (the OS cryptographic source). The small codecs
(take chunk, motif stream, IPC framing, clipboard JSON) are product formats built on
nlohmann/json, xxHash and picosha2.

## Known gaps (not done)

- Clean-VM run, MSI (WiX not installed), code signing, CI never executed.
- Accessibility audit (screen reader, high contrast theme, DPI 125-200%, mixed monitors).
- Preferences file for display octave, theme and import concurrency; only layout, last library
  and shortcut profile persist (QSettings).
- MIDI learn, MIDI Clock send, input channel filters/velocity curves UI, multiple armed tracks,
  punch-out and replace-range controls in the GUI (the recorder supports them; the GUI records
  from the edit cursor with count-in).
- SoundFont playback in the live engine from the GUI (SF2 works in offline rendering).
- Library: file watching (the efsw adapter exists in `platform`), ZIP
  backups, duplicate review/merge, collections and tags UI beyond favorite/rating, saved searches
  UI, export portable library, coalesced change notifications.
- Region/marker editing UI, insert/delete time, split-by-channel/pitch, merge tracks, type-2
  sequence selector UI (the controller supports switching sequences).
- Preview of cached audio (preview cache), library preview audition.
- Undo of catalog metadata changes (recorded but not undoable).
- Still on the GUI thread: the recovery list dialog and recovering a session, and the first
  recovery checkpoint of a new blank document (small). Open, export, save, journal flushes,
  per-edit hashing and checkpoints run on background threads.
- Library analysis of stored revisions and restore parse in process (the bytes were validated at
  import); imports and file open parse in the worker process.
- A library import is one task on the library's write thread; searches and opens use a separate
  read thread and are not delayed, but other writes (saves, tags) wait for the import.
- Retrieval-corpus calibration of similarity and key-estimation confidence.

## UI redesign (after owner feedback)

A UI/UX review (`UI_UX_REVIEW.md`) produced a plan; its P1 items are implemented:

- Base font one point larger (Segoe UI family list), set once at startup.
- Fusion style with exact Dark, Light and High Contrast palettes and a small stylesheet;
  View > Theme (Follow Windows by default, follows Windows changes live); `--theme` for one run.
- One main toolbar built from the commands: new/open/save, undo/redo, the five edit tools
  (keys 1-5, checked state follows B and the menu), a snap selector, transport with painted icons,
  a bar.beat.tick position readout, Drag MIDI, Library/Inspector toggles (Ctrl+1/2/3) and a
  command search box. Tooltips show the current shortcut, also after rebinding.
- Editor-first layout: inspector 300 px, library hidden until a library is opened, track list
  168 px, velocity lane 80 px, on-screen keyboard hidden (View > On-screen Keyboard, or playing
  mode). Layout schema bumped to 2 so old saved layouts do not override it.
- Piano roll: all colors from one theme table, quieter grid with stronger every-fourth bar,
  velocity drives note brightness, selected notes use a light fill with a contrasting outline
  (readable in grayscale), playhead marker in the ruler, an empty-state hint, and opening a file
  fits every note vertically with rows sized to fit (8-16 px).
- Status bar readouts are permanent sections that messages no longer hide; the transport
  position readout now updates (a duplicate label used to replace it).
- Library panel explains what a library is when none is open; the inspector hides placeholder
  values when nothing is selected and reflows instead of scrolling sideways.
- View > Note Colors (track, channel, velocity, scale degree).

Not done from the plan (P2/P3): collapsible inspector sections, bar.beat.tick spin boxes, M/S/R
pill toggles, hover row highlight, F6 pane cycling, Getting Started dialog. Screenshots were
taken offscreen, so glyphs use Qt's fallback font rather than Segoe UI.

## Review follow-up

An independent review (`CODE_REVIEW.md`) scored the code 5/10 with 19 High findings and no
Critical ones. Fixed since, each with a test where one is practical:

| Finding | Fix |
|---|---|
| H1 clipboard JSON could throw out of Ctrl+V | `find` plus type checks, exception barrier; tests with missing keys and wrong types |
| H2 CLI JSON output threw on invalid UTF-8 | replacement error handler; `run()` exception barrier returns the internal-error envelope |
| H3 long multi-byte titles could not be renamed | change-log truncation backs off to a code-point boundary; test with a 360-byte CJK title |
| H4 closing a dirty tab could save or close the wrong tab | saves the asked-about tab, re-resolves its index after the prompt |
| H5 focusing Velocity flattened a mixed selection | only fields changed since the last refresh are applied |
| H6 scroll-bar scrolling left controller lanes on a stale mapping | setters emit `mapping_changed` |
| H7 job coordinator destructor race | destructor waits for the pool |
| H8 huge MIDI files could exhaust memory | events counted before the reader allocates; per-track check; diagnostics capped at 10,000; `bad_alloc` caught; test |
| H10 recipe integers could overflow | recipe integers bounded to 2^40; repeat steps to ±127; tests |
| H11, H12 bulk note edits were O(selection × track) | new `ChangeBuilder::edit_track` merges a whole batch per track; move, paste, insert and duplicate use it; randomized invariant test |
| H14 every edit deep-copied the document for playback | snapshot only while the transport runs; play, play-from-selection and record publish first |
| H15 event list rebuilt on every scrub step | rebuilds only on document or sequence change |
| H16 controller lanes walked every event per paint | walks only the visible range |
| H19 similar-phrase search allocated a map node per posting | flat vote list, one sort, cancellation inside the loop |

H9, H13, H17 and H18 were addressed in the review loop (see below).

## Review loop

Fresh reviewers score code and UI/UX against a fixed rubric each round; findings are fixed,
everything is rebuilt and retested, and the loop continues until both reach 9/10.

| Round | Code | UI/UX | Main fixes after the round |
|---|---|---|---|
| 1 | 5/10 | 6/10 | Split tool (one bulk edit, tests); takes kept when closing during recording; library saves keep the right target; engine/router shutdown order; play waits for the current snapshot; per-session recovery locks so another instance cannot discard live sessions; similar-phrase search merges grams and caps votes; exact search ignores truncated filter grams; loop takes keep the last complete pass; joinable async save stamps modified time; repeat and clipboard bounds; partial analysis runs no longer block full analysis; re-import restores trashed songs. Threading: recovery hashing, checkpoints and flushes, open and export off the GUI thread; library read and write threads; MIDI parsing for open and import in the worker process. UI: single-key shortcuts no longer steal keys from focused controls; Song section (tempo, meter, loop length) and tempo/meter in the transport readout; palette finds themes, panels, loop and metronome; Go to Start (Home); scroll bars with real page sizes; Draw on a note selects it; drum tracks draw on channel 10; lanes show bar lines, a value scale and readout and accept Up/Down; Q no longer steals focus or quantizes an unselected track; the unused R column is gone; trash asks first. New tests: split/update/duplicate, live recovery sessions, worker parser; new fuzz target for recipe and report JSON. |
| 2 | 7/10 | 7/10 | Background-task results post through the application object and check the target, so a closed tab or panel is never touched; library and recovery threads stop with the window (recovery drains queued clean closes); closing a tab no longer flushes and waits; recording is O(change): envelopes are encoded and hashed off the GUI thread on one hashing thread; journals of untouched opened files close cleanly on destruction (no false recovery prompts); library opens, recovery listing, replay and discard run on the I/O thread; library results page with the keyset cursor and report "100+"; a newer search cancels the running one; opens, revisions and drag files use their own library read thread; library open parses in the worker; imports use a second worker so File > Open never waits; binary search in tempo conversions; a worker pipe handle leak on an error path. A crash when closing during a background save was found by a new test and fixed. UI: wheel over unfocused fields scrolls the panel instead of editing; toolbar fits at 1400 px (search in the menu bar corner, fixed-size Drag MIDI); Left/Right move the red transport line by the grid (and seek while playing), Up/Down pick the entry pitch and Return inserts a note; Escape clears, Backspace deletes; note context menu has the edit commands; edge and tool cursors on hover; touchpad scrolling; lane context menu (change target, channel, remove) and no label overlap; per-tool Transform fields with JSON behind a toggle; bar.beat and beat readouts for start and length; high-contrast scroll bars and splitters; quiet tab close buttons; rating submenu; accessible names for M/S; shortcut import and export only fill the table until OK; --workspace keeps settings in that folder. |
| 3 | 7/10 | 7/10 | Drag-file names never split a UTF-8 character and invalid UTF-8 never throws in paths; a failing background task cannot end the process; journal hashing is skipped during edit bursts instead of queueing a document copy per edit (replay checks every recorded hash), checkpoints never block a destructor and a failed checkpoint no longer drops the batch; recovered sessions are discarded only once the new journal exists; worker replies are type-checked (a bad reply fails one file); library threads are owned by the panel, user writes drain on close, imports run on their own thread with progress and Cancel, drag-file preparation is coalesced; documents prepared off the GUI thread never fall back to GUI-thread journal creation. UI: velocity lane keeps Up/Down; Delete works in the event list; unclipped command search; high-contrast spin arrows; stronger dark grid rows; Play button shows Pause while running; keyboard-operable drum grid with a cell cursor; empty-library hint; Transform under Selection and collapsible settings groups; status messages from lanes and the drum grid; menu tooltips explain disabled commands; Ctrl+Left/Right move the play position. |
| 4 | 8/10 | 7/10 | Library rows update in place after favorite, rating, tag, role and trash changes (no re-search, the place and later pages are kept), and the change outbox refreshes rows changed elsewhere every 2 s; level-of-detail drawing in velocity and controller lanes above one item per pixel column; the event list rebuilds once per edit burst and keeps its current event; a release-only E100K performance test (edit and undo of 3,125 notes with the editor's cache rebuild: 27.6 ms median, target 50 ms); type-checked IPC chunk fields; the snapshot compiler cancels on shutdown; bars and beats are at least one tick; track moves check the moved track's identity; a journal encode failure keeps its checkpoint; jobs catch every exception; external MIDI devices open on the compiler thread outside the send lock. UI: Space is Play everywhere except text fields; the drum grid follows the active track, keeps Space for Play (Enter toggles), a double-click no longer edits, lanes hide under it, Ctrl+G toggles it; a right-click in a lane opens its menu (erasing needs a right-drag); inspector sections fold under arrow buttons; the command search keeps the typed text; Delete track is disabled for the last track. |

## Performance (release build, this machine)

Catalog, 100,000 synthetic rows (`docs/testing/bench-catalog-100k.json`), p95: first title page
0.04 ms, deep page 0.04 ms, tag filter 1.42 ms, FTS + bpm (title order) 5.20 ms, FTS + bpm
(relevance) 49.27 ms, change polling 12.72 ms, job lease 2.04 ms. Target ≤150 ms: met.

Library L100K with M10K melody streams (`mwb-cli bench --rows 100000 --motif-rows 10000`, the
release-only `perf.perf_library` test), p95: first title page 0.51 ms, tag filter 2.59 ms, FTS +
bpm 5.25 ms (relevance 31.3 ms), exact melody search of 8 and 32 notes 22 ms, similar search of 16
notes 134 ms. Targets 150 ms / 500 ms / 2 s: met. Before the motif statements were pinned to the
`asset_head` index, SQLite walked every active asset per posting and stream: exact 1 s and
similar 30 s at this size. M10K alone (10,000 assets): exact 5.8 ms, similar 121 ms, browse-index
pages 0.3-0.9 ms.

In-memory browse index (section 19.4): built on the library panel's maintenance thread after
open, kept current from the change outbox before every page, used for filters and title/date
sorts; relevance, non-ASCII text, a loading or over-budget index use SQL. Measured 968 bytes per
row (9.7 MB at 10K), so the 64 MiB default holds about 69,000 items; at L100K the build stops
after its first page (projected size over budget) and SQL paging, measured above, serves the
library. This is the spec's reconsideration trigger ("measured budget breach at the target
corpus"): a compact row format (enum origins, float facts, one token vector) would bring L100K
under 64 MiB; until then the budget is kept and the SQL fallback carries large libraries.

Editor, E100K (`docs/testing/bench-e100k.json`, rerun after the review fixes), medians: note
pairing 18.9 ms, interval index 7.4 ms, viewport query 0.007 ms, preparing a 3,125-note transform
move 88 ms, core drag of 3,125 notes (`update_notes`) 30 ms, pasting 3,125 notes 4.3 ms, applying
a move 1.7 ms, SMF export 23 ms, SMF import 69 ms, native serialize 1,409 ms, native load and
validate 425 ms. Open E100K
≤2 s: met for SMF; native save exceeds a comfortable GUI budget (see gaps).

Not measured: startup p95, GUI frame times, input-to-sound latency, MIDI jitter, endurance.
