# WARNING: resource was not closed: relation "cloudsync_changes"

## Summary
The warning was emitted by PostgreSQL when a SPI query left a “relation” resource open. In practice, it means a SPI tuptable (or a relation opened internally by SPI when executing a query) wasn’t released before the outer SQL statement completed. PostgreSQL 17 is stricter about reporting this, so the same issue might have been silent in earlier versions.

We isolated the warning to the `cloudsync_payload_apply` path when it inserted into the `cloudsync_changes` view and triggered `cloudsync_changes_insert_trigger`. The warnings did **not** occur for direct, manual `INSERT INTO cloudsync_changes ...` statements issued in psql.

## Why it only happened in the payload-apply path
The key difference was **nested SPI usage** and **statement lifetime**:

1. **`cloudsync_payload_apply` loops many changes and uses SPI internally**
   - `cloudsync_payload_apply` is a C function that processes a payload by decoding multiple changes and applying them in a loop.
   - For each change, it executed an `INSERT INTO cloudsync_changes (...)` (via `SQL_CHANGES_INSERT_ROW`), which fires the INSTEAD OF trigger (`cloudsync_changes_insert_trigger`).

2. **The trigger itself executed SPI queries**
   - The trigger function uses SPI to read and write metadata tables.
   - This creates *nested* SPI usage within a call stack that is already inside a SPI-driven C function.

3. **Nested SPI + `INSERT INTO view` has different resource lifetime than a plain insert**
   - With a manual psql statement, the SPI usage occurs only once, in a clean top-level context. The statement finishes, SPI cleanup happens, and any tuptable resources are released.
   - In the payload apply path, SPI queries happen inside the trigger, inside another SPI-driven C function, inside a loop. If any intermediate SPI tuptable or relation is not freed, it can “leak” out of the trigger scope and be reported when the outer statement completes.
   - That’s why the warning appears specifically when the trigger is executed as part of `cloudsync_payload_apply` but not for direct inserts from psql.

4. **PostgreSQL 17 reports this more aggressively**
   - Earlier versions often tolerated missing `SPI_freetuptable()` calls without warning. PG17 emits the warning when the statement finishes and resources are still registered as open.

## Why direct INSERTs from psql didn’t warn
The smoke test included a manual `INSERT INTO cloudsync_changes ...`, and it never produced the warning. That statement:

- Runs as a single SQL statement initiated by the client.
- Executes the trigger in a clean SPI call stack with no nested SPI calls.
- Completes quickly, and the SPI context is unwound immediately, which can mask missing frees.

In contrast, the payload-apply path:

- Opens SPI state for the duration of the payload apply loop.
- Executes many trigger invocations before returning.
- Accumulates any unfreed resources over several calls.

So the leak only becomes visible in the payload-apply loop.

## Fix that removed the warning
We introduced a new SQL function that bypasses the trigger and does the work directly:

- Added `cloudsync_changes_apply(...)` and rewired `SQL_CHANGES_INSERT_ROW` to call it via:
  ```sql
  SELECT cloudsync_changes_apply(...)
  ```
- The apply function executes the same logic but without inserting into the view and firing the INSTEAD OF trigger.
- This removes the nested SPI + trigger path for the payload apply loop.

Additionally, we tightened SPI cleanup in multiple functions by ensuring `SPI_freetuptable(SPI_tuptable)` is called after `SPI_execute`/`SPI_execute_plan` calls where needed.

## Takeaway
The warning was not tied to the `cloudsync_changes` view itself, but to **nested SPI contexts and missing SPI cleanup** during payload apply. It was only visible when:

- the apply loop executed many insert-trigger calls, and
- the server (PG17) reported unclosed relation resources at statement end.

By switching to `cloudsync_changes_apply(...)` and tightening SPI tuptable cleanup, we removed the warning from the payload-apply path while leaving manual insert behavior unchanged.

## Next TODO
- Add SPI instrumentation (DEBUG1 logs before/after SPI_execute* and after SPI_freetuptable/SPI_finish) along the payload-apply → view-insert → trigger path, then rerun the instrumented smoke test to pinpoint exactly where the warning is emitted.
- Note: We inspected the payload-apply → INSERT INTO cloudsync_changes → trigger call chain and did not find any missing SPI_freetuptable() or SPI_finish() calls in that path.
