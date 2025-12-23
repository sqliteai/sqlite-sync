# CODEX.md

Guidance for Codex agents working in this repository.

## Reference

- For full architecture/build/performance details, read `AGENTS.md`.
- Comments and documentation must be written in English unless explicitly instructed otherwise (even if prompts use another language).
- Table names to augment are limited to 512 characters; size SQL buffers accordingly.
- Prefer static buffers with `sqlite3_snprintf` for SQL construction when practical (e.g., fixed pattern + table name in a 1024-byte buffer) instead of dynamic `sqlite3_mprintf`.
- Parameterless SQL should live as global constants in `database_<engine>.c` and be imported via `extern`; parameterized SQL belongs in database-layer functions so each backend can build it correctly.

## Workflow Expectations

- Use `rg`/`rg --files` for search; avoid slow scans.
- Default to ASCII; only introduce non-ASCII if already used and necessary.
- Keep changes tight; add comments only when code is non-obvious.
- Do not revert unrelated user changes or use destructive git commands.
- Prefer `apply_patch` for single-file edits; avoid for generated outputs.

## Build & Test

- Build: `make` (outputs `dist/cloudsync.*`).
- Test: `make test` (builds extension + unit tests). No network expected.

## Hot-Path Notes

- Hot-path code (triggers, merge, commit hooks) must use prepared statements stored on the table context; never compile SQL at runtime in these paths. See `cloudsync.c` and `dbutils.c`.

## SQL Function/File Pointers

- New SQLite functions: implement in `src/sqlite/cloudsync_sqlite.c`, register in `cloudsync_register()`, document in `API.md`, test in `test/unit.c`.
- CRDT merge logic: `src/cloudsync.c` (`merge_insert`, `merge_insert_col`).
- Database abstractions: `src/database.h`, with implementations in `src/sqlite/database_sqlite.c` (SQLite) and `src/postgresql/database_postgresql.c` (PostgreSQL).

## Ask/Escalate When

- Network or privileged commands are needed, or a command fails due to sandbox.
- The workspace is dirty in unexpected ways or destructive actions are requested.
