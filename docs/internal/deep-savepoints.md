# PostgreSQL savepoint depth

A heap scan feeding `cloudsync_payload_apply(payload)` failed with `buffer pin ... is not owned by resource owner SubTransaction` at 126 user savepoints. The caller's resource owner and memory context were stored in a 128-entry array; Cloudsync's internal savepoints consumed additional levels and silently exceeded the array.

The backend now keeps one dynamically allocated frame per Cloudsync subtransaction. Each frame records the caller's owner, memory context and subtransaction ID. Commit and rollback restore a local copy after PostgreSQL ends the subtransaction. Subtransaction callbacks discard frames on both normal completion and external abort; transaction callbacks clear the stack before its owning memory context disappears. Snapshot replacement remains restricted to the outermost Cloudsync savepoint.

This removes Cloudsync's fixed depth cap; PostgreSQL's own resource limits still apply. Allocations happen before opening a new subtransaction.

## Validation

`test/postgresql/63_deep_savepoints.sql` is included in `full_test.sql`. It exercises depths 1, 125, 126, 127, 128, 256, 1024 and 2048, reads payloads from a heap table, checks rollback, reapplies in the same transaction and commits. It also catches 100 trigger failures at depth 256 and verifies a subsequent successful apply in the same backend.

The full suite passes all 546 checks on PostgreSQL 15.19, 17.11 and 18.6. Restoring the old implementation reproduces the buffer-owner error at depth 126. These are local database tests, not tests of a deployed cloud server.
