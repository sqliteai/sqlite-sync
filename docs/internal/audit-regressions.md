# Repository audit fixes

The previously reported `MAX_PARAMS` issue is outside this change.

## Compatibility and limits

- PK doubles retain their deployed little-endian IEEE754 bytes. The old code
  combined host conversion with manual big-endian serialization; unconditional
  byte swapping now makes the historical format explicit on every architecture.
  Integers keep their existing encoding. No migration is needed for supported
  little-endian deployments.
- Decompressed payloads are limited to 256 MiB before allocation. Builds may
  override `CLOUDSYNC_MAX_PAYLOAD_EXPANDED_SIZE`; LZ4's `INT_MAX` bound still
  applies. Existing default chunk sizes are below this limit. Oversized legacy
  monolithic payloads must be rechunked or used with an explicitly raised limit.
- Curl requests now have a 30-second connection deadline and a 300-second total
  deadline, including reused handles. Build overrides are
  `CLOUDSYNC_CONNECT_TIMEOUT_SECONDS` and `CLOUDSYNC_REQUEST_TIMEOUT_SECONDS`.
- Failed payload writes report an error and do not advance the receive cursor.
  PostgreSQL's explicit RLS WITH CHECK rejection remains a skippable policy
  outcome, distinct from generic SQL/permission errors; the call still reports
  processed rows, but does not advance the cursor when a policy denied rows.
  SQLite retains its existing per-group partial-application behavior.
- Test database files live in a private per-run temporary directory, not HOME.
  The harness deletes only its own flat test files at shutdown.

## Regression coverage

| Area | Focused coverage |
| --- | --- |
| 64-bit clocks | Incoming column/database versions, causal length and sequence above UINT32_MAX |
| Double encoding | Golden bytes, decoding a deployed fixture, negative-value roundtrip; forced big-endian conversion build |
| Virtual-table planner | Unusable/unsupported constraints before an accepted constraint; no accepted constraints |
| Payload failures | First, middle and final PK errors; checkpoint unchanged; allocation limits |
| Metadata refill | Trigger rejects insertion of a missing column clock |
| Block LWW | Insert/update rollback on block write failure; allocation failure at each split/list/diff allocation |
| PostgreSQL ownership | Block failure while another SPI cursor is active; no invalid tuple-table cleanup |
| JSON | Root-only member lookup, string values resembling keys, nested keys, Unicode and invalid surrogates |
| Curl | Stalled loopback HTTP server, unpooled handle and pooled handle before/after reset |
| Node | ia32 rejection on all OS families and preservation of x64/arm64-musl selection |
| Fractional indexing | 4096-byte common prefix plus the existing module suite |
| Test harness | Temporary directory cleanup, memory accounting and sanitizer-safe RowID generation |

Run `make unittest`, `make endian-unittest`, `make network-unittest`, and
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
