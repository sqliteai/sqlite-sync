# Repository audit fixes

The previously reported `MAX_PARAMS` issue is outside this change.

## Compatibility and limits

- PK doubles retain their deployed little-endian IEEE754 bytes. `make unittest-s390x`
  runs the SQLite suites on s390x (Docker + QEMU): the golden-byte checks fail there with
  the pre-1.1.4 host-dependent encoding, which a little-endian host cannot detect.
  No migration is provided for data written by earlier versions on big-endian hosts:
  no such deployment exists. The old code
  combined host conversion with manual big-endian serialization; unconditional
  byte swapping now makes the historical format explicit on every architecture.
  Integers keep their existing encoding. No migration is needed for supported
  little-endian deployments.
- A compressed payload's declared expanded size must not exceed what its compressed
  bytes can decompress to under LZ4's 255:1 maximum ratio (plus a small constant), nor
  LZ4's `INT_MAX` API limit. There is no fixed size cap, so large payloads produced by
  `cloudsync_payload_encode` / `cloudsync_payload_save` remain loadable; a forged header
  can no longer trigger a large allocation. A genuine repetitive 8 MiB value compresses
  to 254.3:1, inside the bound.
- Curl requests now have a 30-second connection deadline and a 300-second total
  deadline, including reused handles. Build overrides are
  `CLOUDSYNC_CONNECT_TIMEOUT_SECONDS` and `CLOUDSYNC_REQUEST_TIMEOUT_SECONDS`.
- Payload writes that fail on their data (constraint, raising trigger, type
  error) are skipped, logged as a warning and counted (`receive.failed`), and the
  receive cursor advances: they fail identically on every retry. Transient
  failures (busy/locked, deadlock, serialization failure, cancel, out of memory or
  disk, I/O) and, on PostgreSQL, failures not contained by a savepoint fail the
  apply and leave the cursor in place. PostgreSQL's RLS WITH CHECK rejection stays
  a separate outcome: every row is applied in its own savepoint so a denial raised
  inside the cloudsync_changes trigger (block columns, GOS tables) is contained too;
  denied rows are retried after the rest of the payload while retries make progress,
  then skipped with one summary WARNING. `receive.denied` was removed: denials never
  occur on the SQLite client where the network functions run.
- Block materialization writes a row's pending columns first, and block and GOS
  column writes update an existing row in place, so they pass INSERT policies and NOT
  NULL constraints on other columns. An update that changes no row falls back to the
  upsert, so a row hidden by an UPDATE policy is reported instead of silently skipped
  (`databasevm_changes` exposes the affected row count on both backends).
- PostgreSQL savepoints restore the caller's resource owner and memory context, so
  a payload applied from a table scan no longer trips a foreign buffer pin.
- Block-column migration skips tracked rows whose base row cannot be read.
- Each PK group of a payload is applied under one savepoint (`cloudsync_merge_group`)
  that also covers the metadata its rows write before the flush (sentinel, zeroed
  clocks, block values, winner clocks); a failed flush rolls it all back, so a retried or
  re-delivered row applies cleanly. The flush uses that savepoint instead of its own.
  Skipped entries are counted from `merge_pending_batch.rows` (payload rows that joined
  the batch, including an explicit sentinel, excluding one implied by a column row).
- Only the outermost savepoint opened by cloudsync swaps the active snapshot when it
  ends; nested ones advance the command counter, keeping the caller's snapshot stack
  balanced when an enclosing subtransaction rolls back.
- Every block materialization failure goes through one exit that names the stage,
  column and table and composes the database error; allocation failures clear any
  unrelated database error first so they are not misreported.
- Errors keep their origin. PostgreSQL records the SQLSTATE of every caught error on
  the context (`cloudsync_sqlstate`), preserved across savepoint rollbacks, and every
  `ereport` built from the context error uses it (internal_error only for cloudsync's
  own failures). The SQLite tracking triggers report cloudsync's message and the real
  result code. A missing privilege (`42501` outside a policy check, `SQLITE_PERM` /
  `SQLITE_AUTH`) or a read-only database is not skipped by a payload apply.
- Test database files live in a private per-run temporary directory, not HOME.
  The harness deletes only its own flat test files at shutdown.

## Regression coverage

| Area | Focused coverage |
| --- | --- |
| 64-bit clocks | Incoming column/database versions, causal length and sequence above UINT32_MAX |
| Double encoding | Golden bytes, decoding a deployed fixture, negative-value roundtrip; the unit and regression suites on a real big-endian host (`make unittest-s390x`, s390x under QEMU) |
| Virtual-table planner | Unusable/unsupported constraints before an accepted constraint; no accepted constraints |
| RLS denials | Block-column and GOS denials skipped; every column of a permitted block/GOS row written; a column hidden from UPDATE not recorded as applied; order-dependent denials applied on retry in both the batched and trigger paths; permanently denied rows skipped with checkpoint advanced |
| Block materialization errors | Write failure via cloudsync_text_materialize keeps cause, code/SQLSTATE and names stage, column, table; single-shot allocation failure at every allocation never yields a blank error |
| Error origin | SQLSTATE 40001/23505 through the block and metadata triggers; 40001 and a non-policy 42501 through payload apply, not skipped; SQLite trigger failure keeps SQLITE_CONSTRAINT and names column and table |
| Group atomicity | Resurrected row rejected (data and transient failure): no metadata left, re-delivery creates it, 3 entries counted; existing row keeps its clocks; RLS retry of a resurrected row; apply inside an aborted caller subtransaction |
| Payload failures | First, middle and final PK errors skipped with a warning and checkpoint advanced; locked database fails and keeps the checkpoint (SQLite rollback journal, WAL, PostgreSQL lock_timeout); expanded-size bound (forged 4GB/268MB headers, exact 255:1 boundary, genuine 254:1 payload) |
| Metadata refill | Trigger rejects insertion of a missing column clock |
| Block LWW | Insert/update rollback on block write failure; migration past orphaned metadata; allocation failure at each split/list/diff allocation |
| PostgreSQL ownership | Block failure while another SPI cursor is active; no invalid tuple-table cleanup |
| JSON | Root-only member lookup, string values resembling keys, nested keys, Unicode and invalid surrogates |
| Curl | Stalled loopback HTTP server, unpooled handle and pooled handle before/after reset |
| Node | ia32 rejection on all OS families and preservation of x64/arm64-musl selection |
| Fractional indexing | 4096-byte common prefix plus the existing module suite |
| Test harness | Temporary directory cleanup, memory accounting and sanitizer-safe RowID generation |

Run `make unittest`, `make unittest-s390x`, `make network-unittest`, and
`make -C modules/fractional-indexing/test run`. The network test needs permission
to bind a loopback socket; it does not contact an external service. Run Node
checks from `packages/node` with `npm test -- --run`, `npm run typecheck`, and
`npm run build`.

PostgreSQL's `test/postgresql/full_test.sql` includes the focused audit cases in
`57_audit_regressions.sql`. Run it only against a disposable PostgreSQL instance:
the existing suite creates and drops its test databases.

The fractional-indexing changes and its new test are inside a Git submodule;
they must be recorded there before updating the parent repository's submodule
pointer when preparing a commit.

## Validation performed (2026-09-11)

- macOS arm64: all 150 existing unit checks and the new audit regressions passed.
- The same SQLite suites passed AddressSanitizer and UndefinedBehaviorSanitizer
  with `UBSAN_OPTIONS=halt_on_error=1`, including zero outstanding SQLite memory.
- The forced big-endian conversion regression build passed.
- All 7 network tests and all 340 fractional-indexing module tests passed.
- PostgreSQL 17, rebuilt in an isolated Linux container: 479 reported checks
  passed across 57 test groups, with zero failures and no SPI cleanup warnings.
- Node: 14 tests, TypeScript checking, and CJS/ESM/declaration builds passed.
- PostgreSQL migration compatibility check and Git whitespace checks passed.

No live cloud-service integration was run. Windows, Android and WebAssembly
runtime suites were not executed. The x86_64 runtime attempt was unavailable on
this host (`Bad CPU type in executable`, Rosetta not available). The endian
test exercises conversion logic; it is not a substitute for real big-endian
hardware testing.

Update: `make endian-unittest` has since been removed. Once `pk.c` stopped using
host-order conversions, forcing `__BYTE_ORDER__` produced a byte-identical object, so
it could no longer fail; the host-order helpers it exercised are gone from
`cloudsync_endian.h`. Big-endian coverage is now `make unittest-s390x`: the SQLite unit
and regression suites run on s390x under QEMU, and with the pre-1.1.4 double encoding
restored they fail there on the golden bytes while passing on little-endian hosts.
