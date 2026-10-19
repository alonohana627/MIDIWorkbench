---
name: cli-end-to-end-check
description: Exercise the built CLI and the installed application end to end.
---
# CLI end-to-end check

Files: none changed.

1. `build/release/mwb-cli.exe create --output %TEMP%\a.mid`, then `inspect`, `validate --strict`,
   `convert --profile canonical`, `render --format pcm16`.
2. `mwb-cli library import --library %TEMP%\lib --from <folder>`, then `library analyze`,
   `library search --query "notes:>=1"`, `library melody --notes "C4 E4 G4"`,
   `library backup`, `library verify --objects`.
3. Installed copy: `cmake --install build/release --prefix stage`, then with only
   `C:\Windows\System32` on PATH run `stage\mwb-cli.exe capabilities --json` and
   `stage\MIDIWorkbench.exe --exit-when-ready --workspace %TEMP%\ws` with
   `QT_QPA_PLATFORM=offscreen`, `MWB_AUDIO_BACKEND=null`, `MWB_MIDI_BACKEND=null`.

Verify: every command exits 0 and prints `"status": "ok"`; the GUI exits 0 within a minute.
