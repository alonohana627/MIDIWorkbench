# mwb-cli

Headless access to the same implementations the app uses. Machine output is versioned JSON on
stdout (`"schema_version": 1`, `"status"`, `"command"`, `"warnings"`, `"errors"`); diagnostics go
to stderr. File-writing commands refuse to overwrite unless `--overwrite` is given. Paths are taken
literally (Unicode is fine).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | invalid arguments (including a malformed search query) |
| 2 | unsupported or invalid data |
| 3 | filesystem or database error (missing file, destination exists, library busy) |
| 4 | audio or MIDI device error |
| 5 | internal failure |
| 6 | cancelled |
| 7 | partial batch success (some items of an import failed) |

`--warnings-as-errors` turns warnings into exit code 2.

## Commands

| Command | What it does |
|---|---|
| `inspect FILE` | Container, format, division, sequences, tracks (channels, programs, notes), tempo range, meters, warnings |
| `validate FILE [--strict]` | Format and document invariants; `--strict` treats warnings as errors |
| `create --output FILE [--ppq 960] [--bpm 120] [--meter 4/4]` | Empty document; `.mwb` writes native, anything else SMF |
| `convert FILE --output OUT [--profile P]` | `preserving` (default), `canonical` (conductor track, repairs reported), `type0`, `original` (exact imported bytes), `native` |
| `transform FILE --recipe R.json --output OUT` | Deterministic recipe; a recipe is one transform object or `{"steps":[...]}` |
| `render FILE --output OUT.wav [--synth builtin\|bank.sf2] [--sample-rate 44100\|48000\|96000] [--format pcm16\|pcm24\|float32] [--tail S] [--normalize]` | Offline render; never sends external MIDI |
| `library import --library DIR --from PATH` | Managed import (creates the library if needed) |
| `library search --library DIR --query Q [--limit N]` | Query language of the README cheat sheet |
| `library melody --library DIR --notes "C4 E4 G4" [--mode absolute\|transposed\|contour\|rhythm\|similar]` | Musical content search; `similar` reports score components |
| `library analyze --library DIR [--profile all\|fingerprints\|motif]` | Builds summaries, fingerprints and motif indexes |
| `library backup --library DIR --output DIR` | Consistent verified backup with manifest |
| `library verify --library DIR [--objects]` | Integrity, foreign keys, object hashes (read-only) |
| `library repair --library DIR --rebuild-fts` | Safety backup, then rebuilds the full-text index |
| `devices` | Audio endpoints and MIDI ports with stable IDs |
| `capabilities` | Build ID, compiler, Qt, dependency versions, SQLite options, backends |
| `bench --library DIR [--rows N]` | Synthetic catalog; times hot queries and checks their plans |

## Recipe example

```json
{"steps": [
  {"format": "mwb.transform", "tool": "quantize", "version": 1,
   "parameters": {"grid": {"step": "1/16", "swing": "1/2", "origin": 0}, "strength": "3/4"}},
  {"format": "mwb.transform", "tool": "humanize", "version": 1,
   "parameters": {"seed": "7", "timing_range": 12},
   "prng": {"name": "pcg32", "version": 1, "seed": "7"}}
]}
```

The easiest way to get a valid step is the app's Inspector: choose a tool and copy the
"All parameters" text. Seeds are strings so 64-bit values survive JSON. Add
`"targets": {...}` (the `selection_to_json` form) to restrict a step to tracks, notes or a range;
by default a step applies to every note of the first sequence.

## Examples

```powershell
mwb-cli inspect song.mid
mwb-cli convert song.mid --output song-type0.mid --profile type0
mwb-cli render song.mid --output song.wav --format pcm16 --tail 3
mwb-cli library import --library D:\MidiLibrary --from D:\Downloads\midi
mwb-cli library search --library D:\MidiLibrary --query "tag:bass bpm:120..130"
mwb-cli library melody --library D:\MidiLibrary --notes "E4 G4 A4 B4" --mode transposed
```
