# Building MIDI Workbench

Windows x64 is the release target. The toolchain is MSYS2 CLANG64: Clang, LLD, LLVM libc++ on the
Universal C Runtime, and the CLANG64 Qt 6 package. Nothing else is installed system-wide; every
other library is vendored in `deps/`.

## One-time setup

1. Install MSYS2 from https://www.msys2.org (default location `C:\msys64`).
2. In an MSYS2 shell, install the CLANG64 packages:

   ```bash
   pacman -S --needed mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-qt6-base mingw-w64-clang-x86_64-qt6-tools
   ```

   Versions verified for this tree: clang 23.1.3, CMake 4.4.4, Ninja 1.13.2, Qt 6.11.2. MSYS2 is
   rolling; release builds archive the exact package files they were built with.
3. Use Python 3 (MSYS2's or a Windows install) for `deps/update_deps.py` and the include-graph test.

## Build and test (PowerShell)

```powershell
$env:MWB_TOOLCHAIN_ROOT = 'C:/msys64/clang64'
$env:PATH = "$env:MWB_TOOLCHAIN_ROOT/bin;$env:PATH"
python deps/update_deps.py --check
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The same commands work in Git Bash with `export PATH=/c/msys64/clang64/bin:$PATH`.

## Presets

| Preset | Build type | Notes |
|---|---|---|
| `debug` | Debug | libc++ debug hardening; `build/debug/compile_commands.json` for clangd |
| `release` | RelWithDebInfo | `-Werror`; used for benchmarks and packaging |
| `sanitize` | Debug | AddressSanitizer + UndefinedBehaviorSanitizer, `-Werror` |
| `fuzz` | Debug | libFuzzer targets in `tests/fuzz/` (GUI off) |
| `coverage` | Debug | LLVM source-based coverage |
| `ci`, `ci-sanitize` | release / sanitize | Qt required (`MWB_REQUIRE_GUI=ON`) |

Output goes to `build/<preset>/`. ThreadSanitizer does not exist for this Windows toolchain; race
checking needs a Linux build of the core.

## Options

| Option | Default | Meaning |
|---|---|---|
| `MWB_WERROR` | OFF (ON in release/sanitize/ci) | Warnings in owned code are errors |
| `MWB_BUILD_GUI` | ON | Build the Qt application when Qt is found |
| `MWB_REQUIRE_GUI` | OFF | Fail configuration when Qt is missing |
| `MWB_BUILD_TESTS` | ON | doctest suites and the include-graph check |
| `MWB_BUILD_FUZZERS` | OFF | libFuzzer targets |

## Tests

Each `tests/<area>_tests.cpp` is one executable; CTest gets one entry per doctest `TEST_SUITE`
(`core.time`, `midi.import`, ...) plus `<area>.unnamed` and `include-graph`. A suite entry that
matches no test case fails. Tests run with `QT_QPA_PLATFORM=offscreen`, `MWB_AUDIO_BACKEND=null`
and `MWB_MIDI_BACKEND=null`, so no device is touched (the platform enumeration test only lists
devices).

## Running headless

```powershell
$env:QT_QPA_PLATFORM='offscreen'; $env:MWB_AUDIO_BACKEND='null'; $env:MWB_MIDI_BACKEND='null'
build/debug/MIDIWorkbench.exe --workspace $env:TEMP\mwb-ws --exit-when-ready
build/debug/MIDIWorkbench.exe --workspace $env:TEMP\mwb-ws --screenshot shot.png --exit-when-ready
build/debug/mwb-cli.exe capabilities --json
```

## Install and package

```powershell
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix stage
cd build/release; cpack            # portable ZIP; an MSI too when WiX is installed
```

`cmake --install` copies the executables, the toolchain DLLs they actually import (resolved by
CMake's runtime dependency set from the CLANG64 `bin` directory), Qt via `windeployqt` without
network/TLS plugins or translations, `licenses/GPL-3.0.txt`, every vendored license, and a
CycloneDX SBOM (`licenses/sbom.cdx.json`). The MSI is per-user, claims no file associations and
leaves libraries, documents and recovery data in place on uninstall. Builds are not code-signed.

Smoke test of an installed copy with only System32 on `PATH`:

```powershell
cmd /c "set PATH=C:\Windows\System32 && set QT_QPA_PLATFORM=offscreen && stage\mwb-cli.exe capabilities --json && stage\MIDIWorkbench.exe --exit-when-ready --workspace stage\ws"
```
