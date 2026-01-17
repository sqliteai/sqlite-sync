# SQLite vs PostgreSQL Parity Matrix

This matrix compares SQLite extension features against the PostgreSQL extension and validates the TODO list in `POSTGRESQL.md`.

## Doc TODO validation (POSTGRESQL.md)

- `pk_decode`: Implemented in PostgreSQL (`cloudsync_pk_decode`).
- `cloudsync_update` aggregate: Implemented (`cloudsync_update_transfn/finalfn` + aggregate).
- `payload_encode` variadic support: Aggregate `cloudsync_payload_encode(*)` is implemented; no missing symbol, but parity tests are still lacking.

## Parity matrix

Legend: **Yes** = implemented, **Partial** = implemented with parity gaps/TODOs, **No** = missing.

### Core + configuration

| Feature / API | SQLite | PostgreSQL | Status | Notes |
| --- | --- | --- | --- | --- |
| cloudsync_version | Yes | Yes | Yes |  |
| cloudsync_siteid | Yes | Yes | Yes |  |
| cloudsync_uuid | Yes | Yes | Yes |  |
| cloudsync_db_version | Yes | Yes | Yes |  |
| cloudsync_db_version_next (0/1 args) | Yes | Yes | Yes |  |
| cloudsync_seq | Yes | Yes | Yes |  |
| cloudsync_init (1/2/3 args) | Yes | Yes | Yes |  |
| cloudsync_enable / disable / is_enabled | Yes | Yes | Yes |  |
| cloudsync_cleanup | Yes | Yes | Yes |  |
| cloudsync_terminate | Yes | Yes | Yes |  |
| cloudsync_set / set_table / set_column | Yes | Yes | Yes |  |
| cloudsync_begin_alter / commit_alter | Yes | Yes | Yes |  |

### Internal CRUD helpers

| Feature / API | SQLite | PostgreSQL | Status | Notes |
| --- | --- | --- | --- | --- |
| cloudsync_is_sync | Yes | Yes | Yes |  |
| cloudsync_insert (variadic) | Yes | Yes | Yes |  |
| cloudsync_delete (variadic) | Yes | Yes | Yes |  |
| cloudsync_update (aggregate) | Yes | Yes | Yes | PG needs parity tests. |
| cloudsync_pk_encode (variadic) | Yes | Yes | Yes |  |
| cloudsync_pk_decode | Yes | Yes | Yes |  |
| cloudsync_col_value | Yes | Yes | Yes | PG returns encoded bytea. |
| cloudsync_encode_value | No | Yes | No | PG-only helper. |

### Payloads

| Feature / API | SQLite | PostgreSQL | Status | Notes |
| --- | --- | --- | --- | --- |
| cloudsync_payload_encode (aggregate) | Yes | Yes | Yes | PG uses aggregate only; direct call is blocked. |
| cloudsync_payload_decode / apply | Yes | Yes | Yes |  |
| cloudsync_payload_save | Yes | No | No | SQLite only. |
| cloudsync_payload_load | Yes | No | No | SQLite only. |

### cloudsync_changes surface

| Feature / API | SQLite | PostgreSQL | Status | Notes |
| --- | --- | --- | --- | --- |
| cloudsync_changes (queryable changes) | Yes (vtab) | Yes (view + SRF) | Yes | PG uses SRF + view + INSTEAD OF INSERT trigger. |
| cloudsync_changes INSERT support | Yes | Yes | Yes | PG uses trigger; ensure parity tests. |
| cloudsync_changes UPDATE/DELETE | No (not allowed) | No (not allowed) | Yes |  |

### Extras

| Feature / API | SQLite | PostgreSQL | Status | Notes |
| --- | --- | --- | --- | --- |
| Network sync functions | Yes | No | No | SQLite registers network functions; PG has no network layer. |

## PostgreSQL parity gaps (known TODOs in code)

- Rowid-only table path uses `ctid` and is not parity with SQLite rowid semantics (`SQL_DELETE_ROW_BY_ROWID`, `SQL_UPSERT_ROWID_AND_COL_BY_ROWID`, `SQL_SELECT_COLS_BY_ROWID_FMT`).
- PK-only insert builder still marked as needing explicit PK handling (`SQL_INSERT_ROWID_IGNORE`).
- Metadata bump/merge rules have TODOs to align with SQLite (`SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION`, `SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION`, `SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID`).
- Delete/tombstone helpers have TODOs to match SQLite (`SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL`, `SQL_CLOUDSYNC_DELETE_PK_EXCEPT_TOMBSTONE`, `SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS`, `SQL_CLOUDSYNC_SELECT_COL_VERSION`).

## Suggested next steps

- Add PG tests mirroring SQLite unit tests for `cloudsync_update`, `cloudsync_payload_encode`, and `cloudsync_changes`.
- Resolve `ctid`-based rowid TODOs by using PK-only SQL builders.
- Align metadata bump/delete semantics with SQLite in `sql_postgresql.c`.
