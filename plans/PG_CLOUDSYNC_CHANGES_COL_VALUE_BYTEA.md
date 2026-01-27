# Plan: PG cloudsync_changes col_value as encoded bytea

Requirements (must hold):
- Keep payload format and pk encode/decode logic unchanged.
- Payloads must be interchangeable between SQLite and PostgreSQL peers.
- PostgreSQL `cloudsync_changes.col_value` should carry the already-encoded bytea (type-tagged cloudsync bytes) exactly like SQLite.
- The PostgreSQL layer must pass that bytea through without decoding; decoding happens only when applying to the base table value type.
- Keeping `col_value` as `text` (and casting in SQL) is not acceptable because `pk_encode` would treat it as `DBTYPE_TEXT`, losing original type info (numbers/blobs/null semantics) and producing payloads that are not portable to SQLite peers.

Goals and tradeoffs for the cached helper approach:
- Goal: preserve SQLite-compatible payloads by encoding `col_value` with the same pk wire format before it reaches the SRF/view layer.
- Goal: avoid per-row plan preparation by caching a `SPIPlanPtr` keyed by `(relid, attnum)` for column lookup.
- Tradeoff: still does per-row SPI execution (can’t avoid row fetch); cost is mitigated by cached plans.
- Tradeoff: uses text parameters and type casts in the cached plan, which is slower than binary binding but simpler and type-agnostic.

Goal: make PostgreSQL `cloudsync_changes.col_value` carry the same type-tagged, cloudsync-encoded bytes as SQLite so `cloudsync_payload_encode` can consume it without dynamic type inference.

## 1) Inventory and impact analysis
- Schema/SQL definition assumes text:
  - `src/postgresql/cloudsync--1.0.sql` declares `cloudsync_changes_srf` with `col_value text`, and the `cloudsync_changes` view is a straight `SELECT *` from the SRF.
- SRF query construction assumes text and uses text filtering:
  - `src/postgresql/cloudsync_postgresql.c` `build_union_sql()` builds `COALESCE((SELECT to_jsonb(b)->>t1.col_name ...), '%s') AS col_value` and filters with `s.col_value IS DISTINCT FROM '%s'`.
  - The empty-set fallback uses `NULL::text AS col_value`.
- INSERT path expects text and re-casts to the target type:
  - `src/postgresql/cloudsync_postgresql.c` `cloudsync_changes_insert_trg` reads `col_value` as text (`text_to_cstring`), looks up the real column type, and casts via `SELECT $1::type` before building a `pgvalue_t`.
- SQL constants and core insert path target `cloudsync_changes`:
  - `src/postgresql/sql_postgresql.c` `SQL_CHANGES_INSERT_ROW` inserts into `cloudsync_changes(tbl, pk, col_name, col_value, ...)`.
  - `src/cloudsync.c` uses `SQL_CHANGES_INSERT_ROW` via the database abstraction, so any type change affects core insert/merge flows.
- Payload encode aggregation currently treats `col_value` as whatever type the query returns:
  - `src/postgresql/cloudsync_postgresql.c` `cloudsync_payload_encode_transfn` wraps variadic args with `pgvalues_from_args`; a `bytea` `col_value` would flow through as `bytea` without special handling, but any text assumptions in callers must be updated.
- Tests/docs:
  - All `cloudsync_changes` tests are in SQLite (`test/unit.c`); there are no PG-specific tests or docs referencing `col_value` type.

## 2) Define encoding contract for col_value (PG)
- Encoding contract (align with SQLite):
  - `col_value` is a `bytea` containing the pk-encoded value bytes (type tag + payload), same as SQLite `cloudsync_changes`.
  - `NULL` uses the same pk-encode NULL marker; no PG-specific sentinel encoding.
  - RLS/tombstone filtering should be done before encoding, or by comparing encoded bytes with the known encoded sentinel bytes.
- PG-side encoding strategy:
  - Add a C helper that takes a `Datum` + type metadata and returns encoded bytes using existing `pk_encode` path (`dbvalue_t` wrapper + `pk_encode`).
  - Avoid JSON/text conversions; the SRF should fetch the base-table `Datum` and encode directly.
  - Compute `col_value` for a given row using:
    - PK decode predicate to locate the row.
    - Column `Datum` from SPI tuple (or a helper function returning `Datum`).
- PG payload encode path:
  - Treat `col_value` as already-encoded bytes; pass through without decoding.
  - Ensure `pgvalues_from_args` preserves `bytea` and `pk_encode` does not re-encode it (it should encode the container row, not the inner value bytes).
  - Avoid any path that casts `col_value` to text in `cloudsync_changes_insert_trg`.

Concrete implementation steps for step 2:
- Add a PG helper to encode a single `Datum` into cloudsync bytes:
  - Implement `static bytea *pg_cloudsync_encode_value(Datum val, Oid typeid, int32 typmod, Oid collation, bool isnull)` in `src/postgresql/cloudsync_postgresql.c` (or a new `pg_encode.c`).
  - Wrap the `Datum` into a `pgvalue_t` via `pgvalue_create`, then call `pk_encode` with `argc=1` and `is_prikey=false`.
  - Allocate a `bytea` with `VARHDRSZ + encoded_len` and copy the encoded bytes; return the `bytea`.
  - Ensure text/bytea are detoasted before encoding (via `pgvalue_ensure_detoast`).
- Add a PG helper to encode a column from a base table row:
  - Implement `static bytea *pg_cloudsync_encode_col_from_tuple(HeapTuple tup, TupleDesc td, int attnum)` that:
    - Extracts `Datum` and `isnull` with `SPI_getbinval`.
    - Uses `TupleDescAttr(td, attnum-1)` to capture type/typmod/collation.
    - Calls `pg_cloudsync_encode_value(...)` and returns the encoded `bytea`.
- Update `build_union_sql()` logic to select encoded bytes instead of text:
  - Replace the `to_jsonb(...)->>t1.col_name` subselect with a SQL-callable C function:
    - New SQL function: `cloudsync_col_value_encoded(table_name text, col_name text, pk bytea) RETURNS bytea`.
  - In C, implement `cloudsync_col_value_encoded` to:
    - Look up table OID and PK columns.
    - Decode `pk` with `cloudsync_pk_decode` to build a WHERE clause.
    - Fetch the row via SPI, extract the target column `Datum`, encode it via `pg_cloudsync_encode_value`, and return `bytea`.
  - This avoids dynamic SQL in `build_union_sql()` and keeps encoding centralized.
- Define behavior for restricted/tombstone rows:
  - If the row is not visible or the column cannot be read, return an encoded version of `CLOUDSYNC_RLS_RESTRICTED_VALUE` (text encoded with pk_encode).
  - If `col_name` is tombstone sentinel, return encoded NULL (match SQLite behavior).
- Ensure payload encode path expects bytea:
  - Confirm `cloudsync_payload_encode_transfn` receives `bytea` for `col_value` from `cloudsync_changes`.
  - `pgvalues_from_args` should keep `bytea` as `DBTYPE_BLOB` so `pk_encode` wraps it as a blob field.

## 3) Update cloudsync_changes schema and SRF/view
- Update `src/postgresql/cloudsync--1.0.sql`:
  - `cloudsync_changes_srf` return type: change `col_value text` -> `col_value bytea`.
  - Regenerate or update extension SQL if necessary for versioning.
- Update `build_union_sql()` in `src/postgresql/cloudsync_postgresql.c`:
  - Replace the current `to_jsonb(...)`/`text` approach with encoded `bytea`.
  - Use the PK decode predicate to fetch the base row and feed the value to the encoder.
  - Keep the RLS/tombstone filtering logic consistent with SQLite semantics.
- Update any SQL constants in `src/postgresql/sql_postgresql.c` that target `cloudsync_changes` to treat `col_value` as `bytea`.

## 4) Update INSERT trigger and payload encode path
- In `cloudsync_changes_insert_trg`:
  - Accept `col_value` as `bytea` (already encoded).
  - Avoid casting to text or re-encoding.
  - Ensure typed `dbvalue_t` construction uses the encoded bytes (or passes through unchanged).
- In `cloudsync_payload_encode`/aggregate path:
  - If it currently expects a text value, adjust to consume encoded `bytea`.
  - Confirm the encoded bytes are fed to `pk_encode` (or the payload writer) exactly once.

## 5) Tests and verification
- Add a PG unit or SQL smoke test that:
  - Inserts rows with multiple types (text, integer, float, bytea, null).
  - Queries `cloudsync_changes` and verifies `col_value` bytea can round-trip decode to the original value/type.
  - Compares payload bytes against SQLite for identical input (if a cross-check harness exists).
- If no PG test harness exists, add a minimal SQL script in `test/` with manual steps and expected outcomes.

## 6) Rollout notes and documentation
- Update `POSTGRESQL.md` or relevant docs to mention `col_value` is `bytea` and already cloudsync-encoded.
- Note any compatibility constraints for consumers expecting `text`.
