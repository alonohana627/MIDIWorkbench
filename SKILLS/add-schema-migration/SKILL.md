---
name: add-schema-migration
description: Change the native document or catalog SQLite schema with a checksummed migration.
---
# Add a schema migration

Files: `include/mwb/document/schemas.hpp` or `include/mwb/library/catalog_schema.hpp`,
`src/document/migration.cpp` (native) or the library catalog connection code, tests.

1. Never edit a released DDL in place. Add the new DDL, bump `user_version`, and add a migration
   step with its checksum to the migration list (`document/migration.hpp` pattern).
2. A newer schema than the code knows is refused for writing; keep that check.
3. Migrations write to a temporary/new file (native) or run after a verified backup (catalog);
   never a partially destructive in-place rewrite.
4. Add fixtures: an old-version file opens and upgrades; a newer-version file is refused; an
   interrupted migration leaves the old file intact.

Verify: `ctest -R "document|library"` passes; the whitelist test still rejects injected objects.
