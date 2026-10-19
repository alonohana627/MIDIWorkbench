---
name: add-transformation
description: Add a note or rhythm transformation with preview, recipe JSON and undo.
---
# Add a transformation

Files: `include/mwb/music/transform.hpp`, the matching `src/music/transform_*.cpp`,
`src/music/transform_json.cpp`, `src/ui/inspector.cpp` (tool list and quick fields),
`tests/music_tests.cpp`.

1. Define `XxxParameters` (with `operator==`) and `Result<TransformPreview> xxx(document, selection, parameters)`
   using `NoteEditor`; fill the `TransformReport` counts honestly (clipped, skipped, collisions).
2. Seeded tools use `DeterministicRandom` only, never std distributions.
3. Add the parameters to the `TransformParameters` variant, `tool_id`, JSON encode/decode.
4. Add it to the inspector tool list.
5. Tests: golden numbers, same seed same output, apply then invert restores `semantic_hash`,
   JSON round trip (the all-tools sweep picks it up).

Verify: `ctest -R music` and `ctest -R ui`.
