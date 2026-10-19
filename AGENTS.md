# Binding rules for anyone changing MIDI Workbench

These rules come from `MIDI_Workbench_Full_Specification.md` (sections 4.4, 19, 32.6, 35.3). Read
that specification, `docs/DEVELOPING.md`, `IMPLEMENTATION_STATUS.md` and the relevant
`SKILLS/<task>/SKILL.md` before changing code.

1. **Dependencies first.** Never hand-roll what a library in `deps/manifest.json` already does
   (MIDI I/O and SMF files: libremidi; audio and WAV: miniaudio; SF2: TinySoundFont; hashes:
   picosha2/xxHash; JSON/MessagePack/CBOR: nlohmann/json; CLI: CLI11; query grammar: PEGTL; PRNG:
   pcg-cpp; queues: moodycamel; pool: BS::thread_pool; logs: spdlog; tests: doctest). A missing
   capability means adding a vendored dependency. Exceptions are recorded in
   `IMPLEMENTATION_STATUS.md` with the owner's approval.
2. **Patches only.** Vendored code under `deps/` is never edited by hand. Changes are patch files in
   `patches/<name>/NNNN-description.patch`, listed in the manifest and applied by
   `python deps/update_deps.py --update <name>`. `python deps/update_deps.py --check` must pass.
3. **Names and doc comments.** Clear unabbreviated names; a `///` comment on every meaningful class
   and non-trivial function.
4. **Layout.** Headers in `include/mwb/<module>/`, only `.cpp` in `src/<module>/`. A module includes
   only the modules declared for it in `CMakeLists.txt`; the `include-graph` test enforces this.
5. **Errors.** `mwb::Result<T>` / `mwb::Status`, `[[nodiscard]]`, stable `ErrorCode`s. A deliberate
   discard is `std::ignore = ...` with a comment, never `(void)`. No exceptions across C callbacks,
   worker IPC or audio APIs.
6. **Tests.** Every behaviour change has doctest coverage in `tests/<area>_tests.cpp` inside a
   `TEST_SUITE`. Tests never sleep a fixed time (`mwb::test::wait_until`). Never relax a test to
   make it pass.
7. **Warnings.** Owned code builds clean with the warning set in `cmake/MwbModule.cmake` and
   `-Werror` (`release`, `sanitize`, `ci` presets).
8. **Platform isolation.** Platform code only in `.cpp` files behind `#ifdef _WIN32` with a POSIX
   branch or null implementation.
9. **Real-time paths.** No allocation, locks, file or database access, Qt calls or logging on the
   audio render or MIDI callback threads.
10. **Docs move with code.** Update the affected docs, skill and `IMPLEMENTATION_STATUS.md` in the
    same change. Never write results nobody observed.
11. **Commits only with the owner's explicit OK** for that commit; long messages saying what
    changed, why, how it was verified (real commands and output) and what is left.
12. **Product invariants.** View settings (scale highlight, fold, zoom) never change events; every
    document edit is one undoable command that commits completely or not at all; original imported
    bytes are never modified; import copies bytes into the managed library; nothing is sent to
    external hardware by default; no network access at runtime.
