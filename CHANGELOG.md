# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added

- **`cloudsync_network_send_changes()` accepts an optional limit on how many local database versions to send**, so a large backlog can be uploaded in bounded steps instead of one batch. A send is all or nothing: the server confirms the window only once every chunk of the batch has applied, and a failed batch is re-sent whole. After a long offline period or a bulk import that batch can be large enough to keep failing, and each attempt re-uploads everything. `cloudsync_network_send_changes(max_db_versions)` sends at most that many local transactions, so each call is an independently confirmed batch and a failure costs one bounded window rather than the whole backlog. Call it repeatedly with the same value until `send.status` leaves `out-of-sync`; `send.localVersion` keeps reporting the newest local version so the remaining backlog stays visible. Received changes share the database version counter, so versions holding no local change are skipped instead of consuming the budget. The no-argument form is unchanged.

### Fixed

- **SQLite: paging a chunked download no longer gets slower with every chunk.** The positional cursor on `cloudsync_payload_chunks` was meant to seek straight to where the previous call stopped, but it stated its resume point only inside `(db_version > ? OR (db_version = ? AND seq >= ?))`, whose two arms carry distinct parameters. SQLite does not derive a range from that, so the scan over `cloudsync_changes` ran with an upper bound only and re-read the window from the beginning on every call, discarding rows until it reached the resume point — making a full drain quadratic in the number of chunks, and long enough on a large tenant to hit a server-side deadline and never complete. An explicit `db_version >= ?` is now stated alongside the disjunction; it selects exactly the same rows and lets the scan seek. Locally, draining a 188-chunk window went from 6110 ms to 210 ms, and the per-chunk cost no longer depends on how large the window is. PostgreSQL was never affected.
- **A row rewritten in one statement no longer keeps the old blocks of a shorter value.** Writing a whole row rewrites its block column from the first position, so any block past the end of the new value stayed stored. On SQLite `INSERT OR REPLACE` skips the old row's delete trigger unless `recursive_triggers` is on, so those leftovers kept their metadata and were delivered as content: replacing `AAA\nBBB\nCCC` with `ZZZ` left `ZZZ\nBBB\nCCC` on the peers and on a later local read. On PostgreSQL they carried no metadata, so peers were unaffected, but they stayed in the blocks table for the life of the row. Blocks the new value does not cover are now retired with it.
- **SQLite: a payload whose commit fails no longer leaves its transaction open.** When `cloudsync_payload_apply` started the transaction itself and the commit then failed — a deferred foreign key violated at commit, or `SQLITE_BUSY` because a reader held the database — the transaction stayed open: the uncommitted rows remained visible on the connection and the next `BEGIN` failed. The failed transaction is now rolled back and the original error is returned. Changes from earlier source versions that were already committed are kept, the receive checkpoint does not move, and the rolled-back rows are no longer counted as applied, so delivering the payload again applies it. A transaction or savepoint opened by the caller is still left to the caller.
- **PostgreSQL: applying a payload read from a table works at any savepoint depth.** Inside 126 or more savepoints, `SELECT cloudsync_payload_apply(payload) FROM some_table` still failed with `buffer pin ... is not owned by resource owner SubTransaction` (and a caught error at that depth could abort an assertion-enabled server): cloudsync recorded the caller's resource owner and memory context for at most 128 nesting levels, counting its own internal savepoints, and silently stopped restoring them beyond that. The fixed limit is gone; only PostgreSQL's own resource limits apply.
- **A received row with a block column is applied all or nothing.** When a row's block value failed to write (a trigger that raises, a constraint, a policy), its ordinary columns and their sync metadata stayed applied, leaving the row with a missing or stale block value. The row's ordinary columns, metadata and blocks now roll back together, the receive checkpoint does not move, and delivering the payload again applies the whole row once the cause is fixed. Rows rolled back this way are no longer counted as applied.
- **A block column rewritten in place is no longer lost by the peers it syncs to.** Rewriting a row that holds a block column without changing its primary key — `INSERT OR REPLACE`, or any write that rewrites the same block positions — marked every rewritten block as deleted in the sync metadata while keeping its content locally. The local row still read correctly, so nothing looked wrong, but the peers received the blocks as deleted and dropped the row. The metadata write now keeps a block live when it is written as live, and deleted when it is written as deleted, instead of flipping the two on every write. The same flip affected a row's deletion marker.

## [1.1.4] - 2026-09-21

### Added

- **Network requests now have deadlines**, where previously a stalled server left a sync call waiting indefinitely. 30 seconds to connect, on every request, name resolution included: libcurl is now built with its threaded resolver, since the synchronous one cannot time out a hung DNS lookup. A network call in flight can also be cancelled with `sqlite3_interrupt()`, and then fails with `SQLITE_INTERRUPT` rather than a generic error (SQLite 3.41 or later). API calls are then capped at 300 seconds of elapsed time. Artifact transfers are bounded on progress instead — they abort after 60 seconds below 1 KB/s — because a large payload on a slow link would otherwise be killed mid-flight by an elapsed-time cap, and because a genuine stall is detected sooner this way. A 1-hour absolute backstop still bounds an artifact transfer that trickles just fast enough to stay alive. Tune them at runtime with `cloudsync_set` (`network_connect_timeout`, `network_request_timeout`, `network_artifact_timeout`, `network_artifact_low_speed_limit`, `network_artifact_low_speed_time`), raising or lowering the defaults, or change the defaults at build time with `CLOUDSYNC_CONNECT_TIMEOUT_SECONDS`, `CLOUDSYNC_REQUEST_TIMEOUT_SECONDS`, `CLOUDSYNC_ARTIFACT_LOW_SPEED_LIMIT`, `CLOUDSYNC_ARTIFACT_LOW_SPEED_TIME` and `CLOUDSYNC_ARTIFACT_TIMEOUT_SECONDS`.
- **A compressed payload can no longer make the extension allocate an arbitrary amount of memory.** The decompressed size is read from the payload header and allocated before decompressing, so a few forged bytes could previously claim gigabytes. A declared size larger than LZ4 can actually produce from the compressed bytes (255:1) is now rejected up front, as is one beyond LZ4's own `INT_MAX` limit, where the previous code handed LZ4 a negative capacity. There is no fixed size limit: any payload the library produces, however large, still applies.

### Changed

- **`cloudsync_payload_apply` stops at the first change it cannot write and returns that error**: a constraint, a trigger that raises, a type error, a row-level security denial, a missing privilege, a lock or serialization failure. No later change in the payload is applied, the change that failed leaves no data or sync metadata behind, and the receive checkpoint does not move, so delivering the payload again once the cause is fixed applies it. On PostgreSQL the error keeps its SQLSTATE (`42501` for a policy denial) and the failing statement is rolled back; previously a row denied by a policy was skipped silently while the rest of the payload applied. Block columns, grow-only-set tables and fragmented values behave the same way.

### Fixed

- **PostgreSQL stale-fragment cleanup makes bounded progress through large backlogs.** Each pass considers at most 64 groups before acquiring advisory locks, so a large backlog no longer exhausts the shared lock table and rolls back all cleanup. Values being applied concurrently remain protected.

- **A received change whose write fails is no longer dropped silently.** Previously the error was discarded and the change was lost without a trace. The apply now fails with that error (see Changed), and the network functions report it as `receive.error`, together with the rows applied before it.
- **A received update to a row that is not there is no longer recorded as applied.** Several columns of an existing row are written with one UPDATE; when it changed nothing (the row was deleted while sync was disabled, or a policy's USING clause hides it), the changes were recorded as applied without being stored. The row is now written with the upsert instead, which inserts it or reports the policy.
- **A receive stream no longer checkpoints past a fragmented value it delivered incompletely.** When the stream's final chunk arrives while a value it delivered is still missing pieces, the call now fails with an error and the checkpoint stays in place. Pieces staged by other streams or direct calls are ignored, so they never block a receive. After any failed receive, the next call downloads the stream again from its first page, which re-checks every value against the whole stream; the checkpoint still moves only when the stream completes.
- **A receive checkpoint that cannot be written is reported** as a receive error instead of being ignored.
- **Applying a fragment directly with `cloudsync_payload_apply` no longer moves the receive checkpoint.** A fragment carries no end-of-stream marker, so completing a value in a direct call could advance the cursor past changes not received yet.
- **Staged fragments are cleaned up without breaking a value in progress.** The cleanup of fragment groups left incomplete for 24 hours removed the old pieces of a value whose delivery had just resumed, so that value could never complete; it now removes a group only when all of its pieces are old. A failing cleanup no longer fails the fragment being applied: it is rolled back and logged. Applying a completed value and removing its pieces now happen together, so a failure to remove them is reported and the value is applied again on the next delivery, instead of the failure being ignored.
- **PostgreSQL: pieces of one fragmented value applied at the same time no longer leave the value unapplied.** The server applies each uploaded chunk as its own job, so two transactions could each stage one piece, see only their own, and both succeed without ever applying the value. Under `READ COMMITTED` they now wait for each other, and the stale cleanup skips a value another transaction is applying. Under `SERIALIZABLE` the conflict fails one transaction with a retryable serialization error. A fragment applied under `REPEATABLE READ` is refused with SQLSTATE `0A000`.
- **PostgreSQL: applying a payload read from a table no longer fails.** `SELECT cloudsync_payload_apply(data) FROM some_table` failed with `buffer pin ... is not owned by resource owner TopTransaction` (and could abort an assertion-enabled server): the internal savepoints left the caller's resource owner and memory context switched. Both are now restored when a savepoint ends.
- **Block columns and grow-only-set tables merge under row-level security and NOT NULL constraints.** Both wrote a received column through an upsert holding only the primary key and that column: PostgreSQL checks an INSERT policy against that proposed row even when it becomes an update, and a NOT NULL column rejects it outright, so the column was never written. A block column's row now has its other received columns written first, and an existing row — of either kind — is updated in place. When the update finds no row to change (the row is gone, or an UPDATE policy hides it), the upsert is used as before, so the write is reported rather than recorded as applied without being stored.
- **A received row whose write fails leaves no partial state behind.** Applying a row wrote part of its metadata — for a deleted-and-recreated row, the new sentinel and the reset column clocks — before the row itself, outside the savepoint that protected the write. When the write then failed, the metadata claimed the row was there: delivered again, even after the cause was fixed (a lock that went away, a policy that now allows it), the row was silently never created, and an existing row kept zeroed clocks. Each primary key's changes are now applied under one savepoint that the failed write rolls back entirely.
- **PostgreSQL: savepoints opened by cloudsync no longer disturb the caller's snapshot.** Ending a nested savepoint swapped the active snapshot inside an enclosing one, so rolling that back could leave the calling statement without its snapshot (an assertion failure on assert-enabled builds).
- **PostgreSQL: errors raised by cloudsync keep the SQLSTATE of the database error behind them.** A failure inside the tracking triggers or `cloudsync_payload_apply` — a serialization failure (`40001`), a deadlock (`40P01`), a unique violation (`23505`), a missing privilege (`42501`) — was re-raised as `XX000` internal_error, so application retry logic and error handling keyed on SQLSTATE could not react to it. cloudsync's own failures are still `XX000`.
- **SQLite: a failed tracking write in the insert, update and delete triggers keeps its result code and context.** It was reported as a bare `SQLITE_ERROR` with SQLite's last message; it now carries the real code (`SQLITE_BUSY`, `SQLITE_CONSTRAINT`, ...) and cloudsync's message.
- **Block column failures name the table and column** they were writing or reading, and what failed (reading, collecting or joining the blocks, writing the column), around the underlying database error. No block-column failure reports a blank message any more: allocation failures say so instead of posing as a read failure, and `cloudsync_text_materialize` keeps the error's result code on SQLite.
- **Converting a column to the block algorithm skips rows whose base row no longer exists** (for example deleted while sync was disabled) instead of failing the conversion.
- **Primary-key doubles keep their deployed little-endian IEEE754 byte order on every architecture.** The previous code combined host conversion with manual big-endian serialization; the historical format is now explicit. Integer keys are unchanged, and no migration is needed for little-endian deployments.
- **PostgreSQL no longer frees a tuple table belonging to another open cursor.** A block write that failed while a second SPI cursor was active could release rows still in use; tuple tables are now owned per statement.
- **Block-level LWW text writes roll back cleanly when a block write fails**, instead of leaving the row partially written.
- **Block-column failures now say which table and column failed**, instead of aborting the statement with a blank message. Reading the row back is part of writing a block column, so a row the session cannot `SELECT` — a row-level security policy narrower for reads than for writes, say — is reported with that cause rather than as an empty "not an error".
- **64-bit clock values above `UINT32_MAX` are handled correctly on incoming changes** — column and database versions, causal length, and sequence.
- **The Node package rejects `ia32` on every operating system** rather than selecting an incompatible binary. `x64` and `arm64-musl` selection is unchanged.

## [1.1.3] - 2026-09-11

### Added

- **`sqlitecloud/sqlite-sync-postgres:18`** — CloudSync on PostgreSQL 18, alongside the existing `:17` and `:15` images. PostgreSQL 18 release artifacts (`cloudsync-postgresql18-*`) are published for Linux x86_64/arm64 and macOS arm64/x86_64, and the test suite runs against 18 in CI.

### Changed

- **The Supabase images now track the base image Supabase currently ships for each major.** `sqlitecloud/sqlite-sync-supabase:17` moves from `17.6.1.071` to `17.6.1.170` and `:15` moves from `15.8.1.085` to `15.14.1.170` — the latter also advances PostgreSQL from 15.8 to 15.14. Both Supabase lines are now on an Alpine userland, so `:<major>` is the only tag you need; the transitional `:17-alpine` tag is retired because `:17` is the same image. Exact-base tags (e.g. `:17.6.1.170`) are still published for stacks that pin one. Supabase base images do not pin the `postgres` user's UID, so moving an **existing** data directory across base families needs a one-time `chown` of the data directory and the `supabase_db-config` volume — see [Self-Hosted Supabase](docs/postgresql/quickstarts/supabase-self-hosted.md) for how to discover the new UID and apply it. The CloudSync extension itself is unchanged.
- **The local build-from-source path (`make postgres-supabase-build`, `docker/postgresql/Dockerfile.supabase`) now works with the Alpine-userland Supabase bases** as well as the Ubuntu ones. The extension is compiled in a dedicated glibc builder stage (official `postgres:<major>` image with PGDG headers, the same toolchain that produces the release artifacts) whose PostgreSQL major version is derived from the base image tag, and `pg_config` is resolved by probing the Nix profile paths instead of a hardcoded Ubuntu-only path. The runtime stage no longer needs a compiler or package manager, so the base userland is irrelevant.
- **The development Docker Compose files mount the Postgres data volume at `/var/lib/postgresql` instead of `/var/lib/postgresql/data`.** PostgreSQL 18 keeps its data in a major-version subdirectory and refuses to start when a volume is mounted at the old path; the parent path works for every supported major. Existing local volumes need to be recreated (`docker compose -f docker/postgresql/docker-compose.yml down -v`).

### Fixed

- PostgreSQL payload apply now supports wide tables by growing prepared-statement parameter storage dynamically instead of rejecting statements with more than 32 bound values.
- The website changelog workflow now triggers on the release tags this repository actually creates (`1.1.3`); it was listening for `v`-prefixed tags and never fired automatically.

## [1.1.2] - 2026-07-13

### Fixed

- Querying the SQLite `cloudsync_changes` virtual table (directly or via `cloudsync_payload_blob_checked()`) on a database whose `cloudsync_table_settings` rows survived while all `*_cloudsync` meta tables were lost (e.g. a dump/restore that skipped them) now raises an actionable error ("cloudsync settings reference tables whose sync metadata is missing ...") instead of a misclassified `SQLITE_NOMEM`, which servers reported as "Not enough memory to execute query".

## [1.1.1] - 2026-07-10

### Fixed

- Querying the SQLite `cloudsync_payload_chunks()` virtual table on a database where cloudsync is not initialized now raises the same actionable error as `cloudsync_payload_apply()` ("cloudsync is not initialized: call SELECT cloudsync_init('<table_name>') ...") instead of a bare "SQL logic error". Errors raised by the internal `cloudsync_changes` scan (including the watermark computation, which previously ignored them) are now propagated with their message instead of being masked.

## [1.1.0] - 2026-07-09

### Added

- **Chunked payload generation** via `cloudsync_payload_chunks()`, available as a SQLite virtual table and as a PostgreSQL set-returning function. The API emits transport-sized payload chunks and transparently fragments oversized BLOB/TEXT values into v3 fragment payloads.
- **`payload_max_chunk_size` global setting** for controlling generated chunk size. The default is 5 MB and values are clamped to the 256 KB technical minimum and the 32 MB technical maximum.
- **`exclude_filter_site_id` argument** for `cloudsync_payload_chunks()`. When set, the function streams changes from every site **except** `filter_site_id`, which is what the `/check` download path needs (a peer must not receive its own changes back). The default (omitted/`false`) preserves the existing single-site behavior. Passing the flag without a `filter_site_id` is an error.
- **`cloudsync_payload_blob_checked()`** scalar function on both SQLite and PostgreSQL. It performs an internal conservative size check before generating one monolithic legacy payload BLOB, allowing `/check` endpoints to support old clients with one SQL round trip while rejecting unsafe responses before payload materialization.
- **`cloudsync_uuid_text()` / `cloudsync_uuid_blob()`** scalar functions on both SQLite and PostgreSQL, converting between the 16-byte binary `site_id` and its canonical UUID string. `cloudsync_uuid_text()` takes an optional `dash_format` argument (default `true`); `cloudsync_uuid_blob()` accepts dashed or undashed, case-insensitive input. These let string-based callers (e.g. the `/check` endpoint) pass a `site_id` to `cloudsync_payload_chunks()`.
- **Payload chunking documentation** in `API.md` and `PERFORMANCE.md`, including the explicit memory note that chunking bounds transport payloads but the database must still materialize a completed single BLOB/TEXT value when it is applied.
- **PostgreSQL `1.0 -> 1.1` upgrade script** (`migrations/cloudsync--1.0--1.1.sql`) for the new chunked-payload SQL surface, so existing deployments can `ALTER EXTENSION cloudsync UPDATE`.
- **`cloudsync_network_receive_changes([max_chunks])`** as the canonical name for receiving changes (the old `cloudsync_network_check_changes()` is retained as a deprecated alias). It drains all currently-available chunks by default; the optional `max_chunks` argument caps how many chunks are applied per call for caller-driven progress/traffic control (the in-memory page cursor persists between calls, so a capped drain resumes where it left off).
- **`chunks` and `bytes` fields** in the `send` and `receive` JSON of the network functions, plus **`complete`** in `receive`. `chunks` is the number of payload chunks sent/applied; `bytes` is the serialized (uncompressed) payload bytes; `complete` is `false` when a chunked download stopped before the final chunk, signalling the caller to call again.

### Deprecated

- **`cloudsync_network_check_changes([max_chunks])`** — use `cloudsync_network_receive_changes()` instead. The old name remains a thin, fully-functional alias and will be removed in a future major version.

### Changed

- `cloudsync_network_receive_changes()` (and its `cloudsync_network_check_changes()` alias) now **drains all available chunks** in one call (it does not wait for not-yet-ready server preparation). Previously each call fetched a single page.
- `cloudsync_network_sync(wait_ms, max_retries)` now drains an entire chunked download in a **single call**, fetching already-available chunks back-to-back with no delay. `wait_ms` / `max_retries` are now spent only while the server payload is not yet ready (HTTP 202), not while paging through chunks that are already available. Previously a multi-chunk stream required several `cloudsync_network_sync()` calls and wasted a `wait_ms` delay on each staged fragment.
- `receive.rows` is now the **cumulative** number of rows applied across all chunks drained in the call (previously only the last page was reported); `receive.tables` likewise reports the union of tables touched across the whole drain.
- `cloudsync_payload_apply()` now accepts legacy payloads, monolithic payloads, and v3 fragment payloads without enforcing the local `payload_max_chunk_size`, preserving compatibility between peers with different settings.
- `cloudsync_network_send_changes()` now streams outgoing changes through `cloudsync_payload_chunks()` instead of first building one monolithic payload. This bounds transport payload size for the built-in network path and lets large rowsets or oversized BLOB/TEXT values flow through the same `/apply` endpoint as regular payloads.
- The built-in `/check` receive path now advertises `X-CloudSync-Capabilities: check-status-response, check-chunks` and can apply cursor-mode pages returned inline as `data.payload` base64 bytes, in addition to larger pages returned as `data.url` download artifacts. Requests send `cursor`; responses provide `nextCursor` when another page is available.
- The chunked-download receive path advances the local receive checkpoint (`check_dbversion` / `check_seq`) **only after a chunk stream has been fully applied**, jumping straight to the stream watermark — never into the middle of a source `db_version`. This mirrors the send path and ensures a stop between chunks cannot skip the un-applied rows of a `db_version` split across chunks on the next `/check` (the server resumes on `db_version > since`, with no intra-version cursor). `cloudsync_payload_apply()` no longer advances the receive checkpoint per applied chunk; the built-in network `/check` path drives it from the server's watermark and final-chunk signal, and falls back to the previous monolithic behavior when the server sends no watermark. Re-delivered rows remain idempotent.

### Fixed

- **PostgreSQL backend crash (segfault) on an error raised after `cloudsync_changes_select()`.** The set-returning function returned via `SRF_RETURN_NEXT` / `SRF_RETURN_DONE` from inside its `PG_TRY` block, which skips `PG_END_TRY()` and leaves `PG_exception_stack` pointing at the function's already-returned stack frame. A later `ereport(ERROR)` in the same query — such as the `cloudsync_payload_blob_checked()` size-limit check — then `siglongjmp()`d into freed stack and crashed the backend. The `SRF_RETURN_*` calls now run after `PG_END_TRY()` so the exception stack is always restored. This is a pre-existing bug, not specific to the chunked-payload work.

## [1.0.20] - 2026-05-26

### Changed

- **Improved network sync performance** by reducing request overhead during `cloudsync_network_send_changes()`, especially for small payloads that can now be applied without the extra upload-URL round trip.
- **Improved repeated sync request latency** by allowing the network layer to reuse HTTP connections across CloudSync API calls.

## [1.0.19] - 2026-05-15

### Added

- **Mac Catalyst support**.

## [1.0.18] - 2026-04-29

### Fixed

- **`cloudsync_network_check_changes()`** no longer errors with `missing 'url' in check response` when the server has not yet prepared any incoming changes for this device. The function now returns the standard "no rows yet" response in that case, so polling loops keep working without spurious errors.

### Added

- **`receive.lastFailure`** JSON field on `cloudsync_network_check_changes()` and `cloudsync_network_sync()`, surfacing the most recent server-side failure of the receive pipeline (e.g. the server failed to prepare the next batch of incoming changes for this device). It complements the existing `send.lastFailure` (server-side apply failures) and `receive.error` (local apply failures on this device), so applications can distinguish "the server has trouble producing my changes" from "I had trouble applying them locally". Each function reports only the failures relevant to its own scope: `cloudsync_network_send_changes()` reports `send.lastFailure`; `cloudsync_network_check_changes()` reports `receive.lastFailure`; `cloudsync_network_sync()` reports both.

### Changed

- Updated the request headers sent to the cloudsync HTTP endpoints (version advertisement, per-endpoint capabilities; legacy `Accept` header removed).

## [1.0.17] - 2026-04-24

### Fixed

- **Confusing errors when `cloudsync_init` was never called**: `cloudsync_changes` (SQLite), `cloudsync_db_version`, `cloudsync_db_version_next`, `cloudsync_set_filter`, `cloudsync_clear_filter`, and `cloudsync_payload_apply` now raise a single actionable message pointing at `SELECT cloudsync_init('<table_name>')` instead of leaking low-level symptoms (`out of memory`, `not an error`, silent `-1`, multi-line "no such table" dumps). The guard runs only on the error branch, so the sync hot path is unaffected.

## [1.0.16] - 2026-04-16

### Fixed

- **WASM crash in `cloudsync_set_column` on existing rows**: Calling `cloudsync_set_column(table, col, 'lww', 'block')` on a table with pre-existing rows crashed the WASM build with a `RuntimeError: function signature mismatch` as soon as the block-index migration tried to allocate memory. `block_init_allocator` was casting `cloudsync_memory_alloc` (a `uint64_t size` function) directly to the fractional-indexing allocator's `void *(*)(size_t)` slot. The cast is a no-op on native platforms where `size_t` is 64-bit, but WASM's `call_indirect` enforces strict type checking — the function is registered as `(i64) -> i32` and called as `(i32) -> i32`, triggering an immediate runtime error. A thin `fi_malloc_wrapper` (mirroring the existing `fi_calloc_wrapper`) now bridges the signatures. Native builds are unaffected.

## [1.0.15] - 2026-04-16

### Fixed

- **Silent receive failures**: When `cloudsync_payload_apply` failed during the receive phase (for example with an unknown schema hash, invalid checksum, or decompression error), the error was stored only on the internal cloudsync context and never propagated to the SQL caller. Both `cloudsync_network_check_changes()` and `cloudsync_network_sync()` silently returned no result. Apply errors are now surfaced as a `receive.error` field in the JSON response.

### Changed

- **Error handling contract**: endpoint/network errors (server unreachable, auth failure, bad URL) always raise a SQL error. Processing errors (`cloudsync_payload_apply` failures) are returned as structured JSON via `receive.error` or `send.lastFailure`, so callers can inspect and log them without try/catch logic.
- **`cloudsync_network_send_changes()` output** now includes a `send.lastFailure` object whenever the server reports one (raw pass-through of the server's `lastFailure` — `jobId`, `code`, `message`, `retryable`, `failedAt`, …), regardless of whether the computed `send.status` is `synced`, `syncing`, or `out-of-sync`. The field is omitted when the server does not report a failure.
- **`cloudsync_network_check_changes()` output** now includes a `receive.error` string when `cloudsync_payload_apply` fails, instead of silently returning NULL. Endpoint/network errors still raise a SQL error.
- **`cloudsync_network_sync()` output** now mirrors the same `send.lastFailure` field and, if the receive phase has a processing error (`cloudsync_payload_apply` failure), returns structured JSON with a `receive.error` string rather than failing silently. The send result is always preserved so callers can tell that their local changes reached the server even when applying incoming changes failed. Endpoint/network errors during the receive phase still raise a SQL error. The receive retry loop breaks immediately on processing errors (a schema-hash mismatch will not heal across retries).

## [1.0.14] - 2026-04-15

### Fixed

- **Stale `cloudsync_table_settings` crash**: Reopening a database that had its base table and `<table>_cloudsync` meta-table dropped without calling `cloudsync_cleanup` crashed with a double-free on `sqlite3_close`. Two bugs were involved: (1) `cloudsync_dbversion_rebuild` returned `DBRES_NOMEM` when `cloudsync_dbversion_build_query` yielded a NULL SQL string (stale row in `cloudsync_table_settings` but no matching `*_cloudsync` table in `sqlite_master`), failing extension init; (2) on init failure `dbsync_register_functions` manually freed the context that SQLite already owned via the `cloudsync_version` destructor, causing a double-free when the connection was later closed. `cloudsync_dbversion_rebuild` now treats a NULL build query the same as `count == 0` (no prepared statement, db_version stays at the minimum and is rebuilt on the next `cloudsync_init`), and the manual free in the error path has been removed.

### Added

- Unit test `do_test_stale_table_settings_dropped_meta` (Stale Table Settings Dropped Meta) covering the drop-base-table + drop-meta-table + reopen scenario.

## [1.0.13] - 2026-04-14

### Fixed

- **Block-level LWW migration**: When `cloudsync_set_column(..., 'algo', 'block')` is called on a table that already has tracked rows, those rows are now immediately migrated into the blocks table. Previously, pre-existing column values were ignored until the next UPDATE, leaving sync state incomplete. The migration uses a two-phase collect-then-write approach to avoid SQLite cursor invalidation and `INSERT OR IGNORE` / `ON CONFLICT DO NOTHING` semantics for idempotency.

### Added

- Unit test `do_test_block_lww_existing_data` (Block LWW Existing Data) verifying block migration on `set_column`, idempotency of repeated `set_column` calls, and correct materialization after update.
- PostgreSQL test `50_block_lww_existing_data.sql` with equivalent coverage for the PostgreSQL backend.

## [1.0.12] - 2026-04-11

### Fixed

- **Settings loader**: Prevent infinite loop in `sqlite3_cloudsync_init` when reopening a database that has a persisted block-column setting. `dbutils_settings_table_load_callback` was calling `cloudsync_setup_block_column`, which `REPLACE`d the same row into `cloudsync_table_settings` while `sqlite3_exec` was still iterating it, re-feeding the rewritten row to the cursor. Added a `persist` flag to `cloudsync_setup_block_column` so the loader replays the in-memory setup without writing back.
- **PostgreSQL tests**: Updated 168 `cloudsync_init` callsites across 43 `test/postgresql/*.sql` files to pass integer flags (`0`/`1`) instead of `true`/`false`, matching the signature change in 1.0.9.
- **CI**: The `postgres-test` job now fails on SQL errors and `[FAIL]` markers. `psql` is run with `ON_ERROR_STOP=on`, `pipefail` is enabled around the `tee`, and the captured log is grepped for `[FAIL]` / `psql ERROR` as a final guard.

### Added

- Unit test `do_test_block_column_reload` (Block Column Reload) that persists a block column with a custom delimiter, closes the database, and reopens it — without the fix this hangs the test process.

## [1.0.11] - 2026-04-11

### Fixed

- **cloudsync_cleanup**: Now also drops the `{table}_cloudsync_blocks` table when the table has block LWW columns configured via `cloudsync_set_column(..., 'algo', 'block')`.

### Added

- Unit test `do_test_block_lww_cleanup` verifying that both `{table}_cloudsync` and `{table}_cloudsync_blocks` are removed after `cloudsync_cleanup`.

## [1.0.10] - 2026-04-08

### Fixed

- **PostgreSQL**: Prevent debug assertion crash on `cloudsync_init` error path (#37).
- **Row filter**: `cloudsync_set_filter` and `cloudsync_clear_filter` now reset the metatable and refill it from scratch, ensuring only rows matching the active filter are tracked for sync (#38).

### Added

- Row filter edge-case test coverage: clear/change filter lifecycle, complex expressions (AND, IS NULL), row enter/exit via UPDATE, composite PK with multi-column filters, multi-table roundtrip sync, and pre-existing data prefill tests for both SQLite and PostgreSQL.

## [1.0.9] - 2026-04-08

### Changed

- **cloudsync_init**: Replaced the `force` boolean parameter with an `init_flags` integer bitmask (`CLOUDSYNC_INIT_FLAG`), allowing fine-grained control over which schema sanity checks are skipped. Existing callers passing `0`/`false` or `1`/`true` remain compatible.
- **API**: Updated `cloudsync_init` SQL signature (PostgreSQL) to accept `integer` instead of `boolean` for the third argument, enabling flag combinations via bitwise OR.

### Added

- `CLOUDSYNC_INIT_FLAG_NONE` (0), `CLOUDSYNC_INIT_FLAG_SKIP_INT_PK_CHECK` (1), `CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_DEFAULT_CHECK` (2), `CLOUDSYNC_INIT_FLAG_SKIP_NOT_NULL_PRIKEYS_CHECK` (4) enum values.
- Documentation for `cloudsync_set_filter` and `cloudsync_clear_filter` in API.md.

## [1.0.8] - 2026-04-03

### Changed

- **CI/CD**: Fix flutter package publish workflow not triggering on new releases.

## [1.0.7] - 2026-04-02

### Fixed

- **Harden table initialization against stale config and error cleanup.**

## [1.0.2] - 2026-03-25

### Fixed

- **Swift Package**: Use binary target and versioned macOS framework for Xcode 26 compatibility.
- **Minor bugs** in tests, docs, and examples related to the 1.0.0 major release.

## [1.0.0] - 2026-03-24

### Added

- **PostgreSQL support**: The CloudSync extension can now be built and loaded on PostgreSQL, so both SQLiteCloud and PostgreSQL are supported as the cloud backend database of the sync service. The core CRDT functions are shared by the SQLite and PostgreSQL extensions. Includes support for PostgreSQL-native types (UUID primary keys, composite PKs with mixed types, and automatic type casting).
- **Row-Level Security (RLS)**: Sync payloads are now fully compatible with SQLiteCloud and PostgreSQL Row-Level Security policies. Changes are buffered per primary key and flushed as complete rows, so RLS policies can evaluate all columns at once.
- **Block-level LWW for text conflict resolution**: Text columns can now be tracked at block level (lines by default) using Last-Writer-Wins. Concurrent edits to different parts of the same text are preserved after sync. New functions: `cloudsync_set_column()` to write individual blocks and `cloudsync_text_materialize()` to reconstruct the full text.

### Changed

- **BREAKING: `cloudsync_network_init` now accepts a `managedDatabaseId` instead of a connection string.** The `managedDatabaseId` is returned by the CloudSync service when a new database is registered for sync. For SQLiteCloud projects, it can be obtained from the project's OffSync page on the dashboard.

  Before:
  ```sql
  SELECT cloudsync_network_init('sqlitecloud://myproject.sqlite.cloud:8860/mydb.sqlite?apikey=KEY');
  ```

  After:
  ```sql
  SELECT cloudsync_network_init('your-managed-database-id');
  ```

- **BREAKING: Sync functions now return structured JSON.** `cloudsync_network_send_changes`, `cloudsync_network_check_changes`, and `cloudsync_network_sync` return a JSON object instead of a plain integer. This provides richer status information including sync state, version numbers, row counts, and affected table names.

  Before:
  ```sql
  SELECT cloudsync_network_sync();
  -- 3  (number of rows received)
  ```

  After:
  ```sql
  SELECT cloudsync_network_sync();
  -- '{"send":{"status":"synced","localVersion":5,"serverVersion":5},"receive":{"rows":3,"tables":["tasks"]}}'
  ```

- **Batch merge replaces column-by-column processing**: During sync, changes to the same row are now applied in a single SQL statement instead of one statement per column. This eliminates the previous behavior where UPDATE triggers fired multiple times per row during synchronization.
- **Network endpoints updated for the CloudSync v2 HTTP service**: Internal network layer now targets the new CloudSync service endpoints, including support for multi-organization routing.
- **NULL primary key rejection at runtime**: The extension now enforces NULL primary key rejection at runtime, so the explicit `NOT NULL` constraint on primary key columns is no longer a schema requirement.

### Fixed

- **Improved error reporting**: Sync network functions now surface the actual server error message instead of generic error codes.
- **Schema hash verification**: Normalized schema comparison now uses only column name (lowercase), type (SQLite affinity), and primary key flag, preventing false mismatches caused by formatting differences.
- **SQLite trigger safety**: Internal functions used inside triggers are now marked with `SQLITE_INNOCUOUS`, fixing `unsafe use of` errors when initializing tables that have triggers.
- **NULL column binding**: Column value parameters are now correctly bound even when NULL, preventing sync failures on rows with NULL values.
- **Stability and reliability improvements** across the SQLite and PostgreSQL codebases, including fixes to memory management, error handling, and CRDT version tracking.
