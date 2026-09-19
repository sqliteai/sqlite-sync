# SQLite payload apply: failed commit cleanup

A deferred foreign-key violation can make the final `RELEASE` fail while leaving the transaction open. Apply returned an error but retained uncommitted rows and metadata, and the next `BEGIN` failed. A reader blocking commit in rollback-journal mode produces a similar failure with `SQLITE_BUSY`.

Error exits now converge on cleanup, preserve the original error, release pending merge allocations and unwind the failed group. If apply started a transaction while the connection was in autocommit mode and its commit fails, cleanup rolls back that owned transaction. `ROLLBACK TO` followed by `RELEASE` is insufficient for a busy commit because the release can fail again. Caller-owned transactions are never rolled back wholesale. Groups committed in earlier source database versions remain applied; rolled-back rows are excluded from the applied count, and the checkpoint does not advance on failure.

The group savepoint must also open successfully before rows are merged. A separate allocation leak found during the stress sweep is fixed: `database_pk_names` now frees the partially built names array if the second schema scan fails.

## Validation

The audit regression suite runs 100 deferred-constraint failure/retry cycles, covering both final commit and a commit at an intermediate source database-version boundary. It verifies data and metadata rollback, retained committed prefixes, unchanged checkpoints, a subsequent unrelated transaction and successful replay. It also tests preservation of a caller-owned transaction and 30 repeated busy-commit failures followed by successful retry.

The core and audit suites pass with AddressSanitizer and UndefinedBehaviorSanitizer, including instrumentation of SQLite itself and zero outstanding SQLite memory. The initial negative control with the old apply implementation failed 400 assertions in the deferred-constraint tests.

## Remaining engine-level OOM limitation

The diagnostic `test/stress/payload_oom.c` fails each allocation of a SQL-function apply in turn, checks transaction state, performs explicit recovery where necessary and verifies retry and memory usage. It deliberately returns nonzero if any transaction remains open; it is not included in the passing regression suite.

The ordinary build sweep covers allocation indices 0–347: zero memory leaks, zero failed retries after recovery, but 126 attempts still leave a transaction open. During these engine-level failures, SQLite sets its connection's malloc-failed/interrupted state and rejects reentrant cleanup SQL while the outer user-defined function is executing. The extension cannot clear that state through the public SQLite API. This PR does **not** claim to fix those 126 cases. With the fully instrumented ASan/UBSan build, the sweep reaches index 351 and reports 128 open transactions, again with zero leaks and zero failed retries after recovery; no sanitizer diagnostic was emitted. Allocation positions depend on build configuration. A safe complete solution requires a host-side recovery boundary or a change to the SQL apply execution model; replacing application trace callbacks or accessing private SQLite state would introduce compatibility risks.

After a `SQLITE_NOMEM` error, the host should reset/finalize the failed statement and inspect `sqlite3_get_autocommit()`. If the host began the operation in autocommit mode and the connection is still in a transaction, it should explicitly roll back before reuse. If the host owns a transaction, recovery must follow its transaction policy rather than blindly rolling back unrelated work.

To reproduce the diagnostic on macOS after building `dist/review_regressions`:

```sh
cc -g -O1 -Isrc -Isrc/sqlite -Isrc/network -Isqlite -Imodules/fractional-indexing \
  -DSQLITE_CORE -DCLOUDSYNC_UNITTEST -DCLOUDSYNC_OMIT_NETWORK \
  test/stress/payload_oom.c \
  $(find build/test -name '*.o' ! -name 'unit.o' ! -name '*bench.o' ! -name 'integration.o' ! -name 'review_regressions.o') \
  -framework Security -o /tmp/payload-oom
/tmp/payload-oom
```

The independent branch also passes all 521 PostgreSQL 15.19 checks, validating the shared apply cleanup. These are local database tests; no deployed cloud server was modified or exercised.
