---
name: add-search-field
description: Add a field to the library query language with SQL and in-memory parity.
---
# Add a search field

Files: `src/search/query_parser.cpp` (grammar and validation), `include/mwb/search/query.hpp`
(field enum), `src/search/sql_compiler.cpp`, `src/search/browse_index.cpp`, `tests/search_tests.cpp`,
`tests/library_tests.cpp`, README cheat sheet.

1. Add the field name and value type to the grammar; validate units and ranges at parse time.
2. Compile it to parameterized SQL only (never concatenate user text). Musical predicates join the
   per-sequence group so AND conditions hold in one sequence.
3. Evaluate the same AST in the browse index with identical semantics.
4. Add the field to the parity test and an EXPLAIN QUERY PLAN check if it is a hot predicate.

Verify: `ctest -R "search|library"`; the parity test compares SQL and index results.
