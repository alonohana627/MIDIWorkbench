---
name: add-dependency
description: Vendor a new third-party library under deps/ with pins, digests and license.
---
# Add a dependency

Files: `deps/manifest.json`, `deps/CMakeLists.txt`, `deps/README.md`, `IMPLEMENTATION_STATUS.md`.

1. Check the license is GPL-3.0 compatible (MIT, BSD, zlib, ISC, BSL, Apache-2.0, LGPL, GPL-3.0,
   public domain).
2. Add an entry to `deps/manifest.json`: name, purpose, version, SPDX license, homepage,
   `license_file`, `source` (`{"type":"github","repo":...,"ref":"vX.Y","commit":"","archive_sha256":""}`
   or a URL with its digest), `include` globs (keep only what is compiled plus the license),
   `patches: []`, `files: {}`.
3. `python deps/update_deps.py --pin NAME`, then `--update NAME`, then `--check`.
4. Add a target in `deps/CMakeLists.txt` with `mwb_vendored_interface` or `mwb_vendored_static`
   (SYSTEM includes, warnings off) and link it from the module that uses it in `CMakeLists.txt`.
5. Add a row to `deps/README.md`. The About box and installer pick it up from the manifest.

Verify: `--check` passes; a clean `release` build has no warnings; `mwb-cli capabilities` lists it.
