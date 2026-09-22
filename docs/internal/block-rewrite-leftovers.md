# Leftover blocks when a whole row is written

Deleting a row keeps the values of its block columns in the blocks table; only their metadata goes away. Writing the row again rewrites the column from its first position, so the new blocks overwrite the old ones position by position and any block beyond the new value's length stays behind.

On SQLite that leaks into the data. `INSERT OR REPLACE` skips the old row's delete trigger while `recursive_triggers` is disabled (the default), so the leftover blocks keep their live metadata: replacing `AAA\nBBB\nCCC` with `ZZZ` left `ZZZ\nBBB\nCCC` on the replicas and on a later local materialization. On PostgreSQL the leftovers carry no metadata, so they never reach a replica, but they stay in the blocks table for as long as the row exists.

`local_block_update` now reads the column's stored blocks on that path too. They stay out of the diff — a whole-row write is not an edit of the previous value — and whatever the new value does not rewrite is tombstoned and its value removed. A failure there is reported like any other block write, so the enclosing statement rolls back instead of committing half a column. The parity-preserving metadata upsert from #46 is required, because the rewritten positions land on rows a previous write tombstoned.

Ordinary INSERT behavior is unchanged, including the convention that NULL block text is stored as one empty block and materializes as empty text.

## Regression coverage

`test/unit.c` runs 120 replacement cycles with recursive triggers OFF and ON, covering shorter, longer, empty, NULL, duplicate-line and trailing-delimiter values. An intervening UPDATE creates fractional block positions. Each cycle checks duplicate payload delivery, materialization on both source and replica, a second block column and an untouched row. An injected block-write failure checks that the replacement rolls back.

`test/postgresql/64_block_rewrite_leftovers.sql` covers the same shape on PostgreSQL: a row recreated with fewer blocks, and an upsert of the whole row, must leave one stored block and deliver the shorter value to a replica.

Both fail against the previous implementation: the SQLite test on content, the PostgreSQL one on the leftover rows.
