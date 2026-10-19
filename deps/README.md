# Vendored dependencies

Every third-party library the build compiles lives here, pinned in `manifest.json` (exact commit or
archive digest, the files kept, patches, and the SHA-256 of every vendored file). Nothing is
downloaded at application runtime.

| Directory | Version | License | Used for |
|---|---|---|---|
| sqlite | 3.54.1 | Public domain | Native documents, catalog and recovery databases (FTS5, STRICT) |
| sqlitecpp | 3.4.0 | MIT | Prepared statements, transactions |
| libremidi | 5.4.3 + 2 patches | BSD-2-Clause | SMF reader/writer, WinMM MIDI device I/O |
| miniaudio | 0.11.25 | MIT-0 | WASAPI output, null backend, WAV encoding |
| picosha2 | 1.0.1 | MIT | SHA-256 |
| xxhash | 0.8.3 | BSD-2-Clause | XXH3-64 n-gram keys and IPC checksums |
| stduuid | 1.2.3 | MIT | UUID text form |
| nlohmann_json | 3.12.0 | MIT | JSON, MessagePack (worker IPC), CBOR (recovery journal) |
| cli11 | 2.7.2 | BSD-3-Clause | Command-line parsing |
| utf8proc | 2.12.0 | MIT | NFKC case folding for search keys |
| pegtl | 3.2.8 | BSL-1.0 | Search query grammar |
| rapidfuzz | 3.3.3 | MIT | Command palette fuzzy matching |
| pcg | 0.98.1 | Apache-2.0 OR MIT | Deterministic PCG32 |
| efsw | 1.7.2 | MIT | Directory change notification |
| readerwriterqueue | 1.0.7 | BSD-2-Clause | Real-time SPSC queues |
| concurrentqueue | 1.0.5 | BSD-2-Clause OR BSL-1.0 | MPSC control queues |
| thread_pool | 5.1.0 | MIT | Bounded job pool |
| spdlog | 1.17.0 | MIT | Rotating logs |
| miniz | 3.1.2 | MIT | ZIP backups |
| doctest | 2.5.3 | MIT | Tests (development only) |
| nanobench | 4.6.0 | MIT | Benchmarks (development only) |
| tinysoundfont | git 853a0a1 | MIT | SoundFont 2 playback |
| signalsmith_dsp | 1.7.3 | MIT | Filters for the built-in synth |
| midifile | git 98917df | BSD-2-Clause | Independent SMF oracle for tests |

System dependencies (not vendored): the MSYS2 CLANG64 toolchain and Qt 6 (see `docs/BUILDING.md`).

## Workflow

```powershell
python deps/update_deps.py --check            # offline: pins, digests, stray files, patches
python deps/update_deps.py --verify-upstream  # compare pinned commits with upstream tags
python deps/update_deps.py --pin NAME         # resolve a new ref to a commit and record the archive digest
python deps/update_deps.py --update NAME      # fetch to staging, verify, patch, then replace deps/NAME
```

To change a library version, edit its `source.ref` and clear `commit` and `archive_sha256` in
`manifest.json`, then `--pin NAME` and `--update NAME`. A moved tag is an error, never followed.

## Patches

Vendored files are never edited by hand. A change is a unified diff in
`patches/<name>/NNNN-short-description.patch` (paths `a/...`/`b/...` relative to `deps/<name>/`),
listed in the dependency's `patches` array, and re-applied by `--update`. `--check` fails if a
patch is missing or a vendored file differs from its recorded digest.

Current patches:

- `libremidi/0001-reader-smpte-unknown-chunks-f7-vlq-meta-lengths.patch`: report SMPTE divisions
  instead of rejecting them; skip non-MTrk chunks and header extensions; keep the F7 marker on
  escape packets; reject variable-length quantities longer than four bytes; accept any length for
  known meta events so the adapter can preserve malformed payloads as evidence.
- `libremidi/0002-writer-format-division-end-of-track.patch`: explicit SMF format (including type
  2), raw division word (SMPTE), and end-of-track placement after the last event.
- `libremidi/0003-reader-capped-track-reserve.patch`: cap the per-track event reservation at
  65,536, so a track that declares a huge length with few events (one big SysEx) cannot reserve
  gigabytes before the payload limits reject it (code-core-13).

Not compiled: libremidi's optional `client.cpp` (fails with this libc++; the application uses
`observer`, `midi_in` and `midi_out` directly). signalsmith-dsp is exposed only through its
`include/` directory because its root contains a `windows.h` that would shadow the SDK header.
