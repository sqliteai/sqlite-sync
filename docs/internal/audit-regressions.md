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
- Curl API requests have a 30-second connection deadline and a 300-second total
  deadline, including reused handles and DNS resolution (threaded resolver). Artifact
  transfers instead have a 1-hour backstop and abort after 60 seconds below 1 KiB/s.
  `cloudsync_set` can tune the deadlines at runtime; `sqlite3_interrupt` cancels a
  transfer on SQLite 3.41 or newer. See the network settings in API.md.
- An apply stops at the first failed write, including constraint/trigger/type errors,
  RLS denials and transient failures. Later changes are not processed and the receive
  checkpoint stays put. PostgreSQL preserves the original SQLSTATE and rolls back the
  failing SQL statement. SQLite can retain earlier successful groups, which re-merge
  on redelivery after the cause is fixed. There is no internal RLS retry loop and no
  `receive.failed` or `receive.denied` counter. Network results report `receive.error`
  and the applied prefix in `receive.rows`.
- Fragment calls stage, reconstruct, apply and remove their pieces under a savepoint.
  Receive streams check their incomplete values before advancing a final watermark;
  direct v3 applies do not move the receive checkpoint. Failed streams restart at page
  zero. A fragmented final chunk without a watermark fails explicitly.
- PostgreSQL serializes concurrent fragments of one value under READ COMMITTED;
  SERIALIZABLE may require a retry, and REPEATABLE READ is refused. Cleanup preserves
  groups another transaction is applying, and only removes groups whose newest piece
  is older than 24 hours. Each cleanup materializes at most 64 candidate groups before
  acquiring advisory locks, preventing a large backlog from exhausting the lock table
  in one maintenance pass. Subsequent calls drain further batches; maintenance remains
  throttled to once a minute per connection. Long caller transactions can still retain
  locks from multiple applies: commit upload jobs individually.
- Block materialization writes a row's pending columns first, and block and GOS
  column writes update an existing row in place, so they pass INSERT policies and NOT
  NULL constraints on other columns. An update that changes no row falls back to the
  upsert, so a row hidden by an UPDATE policy is reported instead of silently skipped
  (`databasevm_changes` exposes the affected row count on both backends).
- PostgreSQL savepoints restore the caller's resource owner and memory context, so
  a payload applied from a table scan no longer trips a foreign buffer pin.
- Block-column migration skips tracked rows whose base row cannot be read.
- Pending ordinary columns and their metadata share a PK-group savepoint, so a
  rejected flush rolls back its sentinel, reset clocks and winner clocks. Before a
  block is materialized, pending ordinary columns are flushed. This is not an atomic
  whole-payload guarantee; on SQLite that earlier flush can survive a later block error.
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
| RLS denials | Block/GOS denials fail with SQLSTATE 42501 and unchanged checkpoint; permitted rows written in full; hidden UPDATE rows not recorded as applied; redelivery after policy/dependency repair; permanent denials keep failing |
| Block materialization errors | Write failure via cloudsync_text_materialize keeps cause, code/SQLSTATE and names stage, column, table; single-shot allocation failure at every allocation never yields a blank error |
| Error origin | SQLSTATE 40001/23505 through the block and metadata triggers; 40001 and a non-policy 42501 through payload apply, not skipped; SQLite trigger failure keeps SQLITE_CONSTRAINT and names column and table |
| Group atomicity | Resurrected row rejected (data and transient failure): no metadata left, re-delivery creates it; existing row keeps its clocks; RLS retry of a resurrected row; apply inside an aborted caller subtransaction |
| Payload failures | First, middle and final PK errors stop the apply with the checkpoint unchanged; locked database fails and keeps the checkpoint (SQLite rollback journal, WAL, PostgreSQL lock_timeout); expanded-size bound (forged 4GB/268MB headers, exact 255:1 boundary, genuine 254:1 payload) |
| Fragment cleanup | 30,000 stale groups; bounded lock count and forward progress across calls; locked and fresh groups preserved; real fragmented value still completes |
| Metadata refill | Trigger rejects insertion of a missing column clock |
| Block LWW | Insert/update rollback on block write failure; migration past orphaned metadata; allocation failure at each split/list/diff allocation |
| PostgreSQL ownership | Block failure while another SPI cursor is active; no invalid tuple-table cleanup |
| JSON | Root-only member lookup, string values resembling keys, nested keys, Unicode and invalid surrogates |
| Curl | Stalled loopback HTTP server, unpooled and reset pooled handles, runtime deadline changes, interruption, paging and fragment replay |
| Node | ia32 rejection on all OS families and preservation of x64/arm64-musl selection |
| Fractional indexing | 4096-byte common prefix plus the existing module suite |
| Test harness | Temporary directory cleanup, memory accounting and sanitizer-safe RowID generation |

Run `make unittest`, `make unittest-s390x`, `make network-unittest`, and
`make -C modules/fractional-indexing/test run`. The network test needs permission
to bind a loopback socket; it does not contact an external service. Run Node
checks from `packages/node` with `npm test -- --run`, `npm run typecheck`, and
`npm run build`.

PostgreSQL's `test/postgresql/full_test.sql` includes the focused audit cases in
`57_audit_regressions.sql` through `61_fragment_cleanup_backlog.sql`. Run it only
against a disposable PostgreSQL instance: the suite creates and drops its databases.

The fractional-indexing changes and its new test are inside a Git submodule;
they must be recorded there before updating the parent repository's submodule
pointer when preparing a commit.

## Validation

Independent review of commit `1820a15` on 2026-09-19:

- PostgreSQL 15.19, 17.11 and 18.6: 516 checks per version, zero failures.
- SQLite macOS arm64: 150 unit checks and the focused regressions passed, including
  the extension core built with AddressSanitizer/UndefinedBehaviorSanitizer.
- Three SQLite replicas: 30,000 randomized writes, 10,800 payload deliveries and 600
  convergence checks, with no divergence or outstanding SQLite allocations.
- 100 randomized fragment trials with duplicates, failure/rollback and replay passed.
- All 14 Linux network test groups and 340 fractional-indexing tests passed.
- Node: 14 tests, TypeScript checking and CJS/ESM/declaration builds passed.
- Migration compatibility (`1.1.3 -> 1.1.4`, unchanged SQL surface) and whitespace passed.

The review found the unbounded cleanup-lock regression now covered by test 61. The
pre-fix query rolls back all maintenance with `out of shared memory` on a 30,000-group
backlog. With the bounded query, all 521 checks pass on PostgreSQL 15.19, 17.11
and 18.6. The new test passes with the fix and fails on the previous query, confirming
that it detects the regression. It checks bounded progress, retained locks, concurrent
protection, the next cleanup batch and completion of the genuine fragmented value.

The review did not contact a live cloud service or run Windows, Android, WebAssembly,
iOS or s390x. `make unittest-s390x` is the big-endian runtime target; the old
`endian-unittest` target was removed because forcing endian macros produced identical
objects and could not detect the bug. See [cloud-e2e.md](cloud-e2e.md) for the existing
real-cloud integration path and how to verify that cases actually ran.

Known pre-existing limits identified during review: a failed deferred-constraint
commit on SQLite can leave a transaction open; PostgreSQL's fixed savepoint-owner
storage does not cover deeply nested callers (the scan/apply reproduction fails at
126 user savepoints); allocation-fault injection still exposes cleanup gaps. These
are not covered by a claim that all error paths or all platforms have been validated.
