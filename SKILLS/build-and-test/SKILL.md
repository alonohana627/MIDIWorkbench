---
name: build-and-test
description: Configure, build and run every test of MIDI Workbench with the MSYS2 CLANG64 toolchain.
---
# Build and test

Files: none changed. Read `docs/BUILDING.md` for options.

1. `export PATH=/c/msys64/clang64/bin:$PATH` (Git Bash) or the PowerShell equivalent.
2. `python deps/update_deps.py --check` must print `OK`.
3. `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`.
4. Before handing work back, also run `cmake --preset release && cmake --build --preset release`
   (warnings are errors there) and `ctest --preset release`.
5. For memory errors: `cmake --preset sanitize`, build, then
   `ASAN_SYMBOLIZER_PATH=C:/msys64/clang64/bin/llvm-symbolizer.exe ctest --preset sanitize`.

Verify: every CTest entry passes, including `include-graph`. Record the real summary lines in
`IMPLEMENTATION_STATUS.md` when they change.
