---
name: add-export-profile
description: Add an SMF compatibility export profile with an explicit change report.
---
# Add an export profile

Files: `include/mwb/midi/codec.hpp` (`CompatibilityProfile`), `src/midi/export.cpp`,
`src/cli/document_commands.cpp` (`convert --profile`), `tests/midi_tests.cpp`, `docs/COMMAND_LINE.md`.

1. Add the enum value and implement it in `sequence_tracks` / `repair_track`; every omitted,
   converted or repaired item goes into `ExportReport` diagnostics with a count.
2. Output must be deterministic for equal input and options; never globally reorder same-tick
   note-offs before note-ons.
3. Tests: re-import the output and compare content; check the report; the midifile oracle reads it.

Verify: `ctest -R midi` and `ctest -R cli`.
