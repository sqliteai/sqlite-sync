# Atomic application of mixed ordinary and block columns

Before materializing a block column, payload apply flushes the row's ordinary columns so the base row exists. Previously this flush also released the PK group's savepoint. A later block-table write error therefore left the ordinary columns and their metadata committed while the block column was missing or stale.

The pre-block flush now writes pending ordinary columns without releasing the group's savepoint or advancing the applied-row count. All blocks and ordinary columns remain inside that boundary until the PK, table or source database version changes. A rejected block rolls back that entire group, including metadata, blocks and resurrection changes. Previously completed groups retain their existing semantics; a failed apply does not advance the receive checkpoint. Fixing the underlying error and replaying the payload restores the complete row.

## Validation

`test_block_group_atomicity` in `test/review_regressions.c` runs 120 failure-and-retry cases across inserts, updates and resurrected rows, 3–10 blocks, rejection of first/middle/last blocks, and both autocommit and caller-owned transactions. It compares both directions of SQL EXCEPT for the base table, metadata and block table, checks the unchanged checkpoint and caller transaction, and verifies successful retry and duplicate delivery.

The core suite and audit regressions pass under AddressSanitizer and UndefinedBehaviorSanitizer, with SQLite itself instrumented and zero outstanding SQLite memory. Restoring the original pre-block flush fails 360 assertions in the new tests. The independent branch also passes all 521 PostgreSQL 18.6 checks for shared-code compatibility. These tests use local databases; they do not validate a deployed cloud server.
