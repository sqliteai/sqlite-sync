# Migration Architecture — Design Specification v2

This document proposes a redesign of the schema-migration subsystem that supersedes `MIGRATION.md`. It addresses a set of critical issues with the v1 proposal:

- The v1 API is C-only and cannot be invoked from SQL consoles, SQLiteCloud server-side execution, or non-C clients.
- v1 positions the server as the migration author and clients as consumers; there is no first-class "develop locally, push to cloud" workflow even though that is the natural motion for every other part of CloudSync.
- v1 specifies no transport — the sync network layer speaks the DML payload protocol, not arbitrary SQL, and no SDK/REST path is defined for fetching or pushing migrations.
- v1 has no adoption path for existing deployments; it assumes a greenfield replay from version 0.
- v1's additive "Tier 1" DDL-in-DML-payload tier is documented but unimplemented.
- v1 forces DDL into a binary descriptor meta-format, duplicating work the database's DDL parser already does.
- v1's `RENAME_COLUMN` operation silently destroys CRDT history because `cloudsync_commit_alter` is schema-diff based and purges shadow rows for the old column name.
- v1 defines no recovery path when a developer bypasses the alter lifecycle and mutates the server schema directly.

The v2 architecture below is structured around five principles.

## Goals

- **SQL-first API.** Every developer-facing and client-facing function is exposed as a SQLite scalar or table-valued function. Same style as `cloudsync_init`, `cloudsync_network_sync`, `cloudsync_begin_alter`, etc. Any C API is an internal implementation detail and is not shipped as the public surface. This means the migration system is fully usable from psql / sqlite CLI / SQLiteCloud consoles / any driver in any language — exactly the places where schema authoring actually happens.
- **Server-authoritative, linear history.** The cloud database is the single source of truth for schema. No CRDT, no logical clocks, no peer-to-peer merge. A total order of migrations flows from one canonical chain.
- **Developer authors natively, with optional manual translation.** DDL is written in real SQLite or PostgreSQL syntax, not a meta-format. The service auto-translates to the other dialect by default; the developer can override the translation inline when the auto-converter would get it wrong or when a statement has no automatic equivalent.
- **Privileged push, automatic pull.** Authoring is an explicit developer action gated by an apikey scope. Consumption by end-user clients is automatic, triggered by normal DML sync.
- **Zero-change onboarding.** Rolling out the feature must not require any modification to an existing CloudSync deployment. A database already in production becomes migration-aware by snapshotting its current schema as the baseline.
- **Preserve CRDT history across renames.** The alter lifecycle must translate shadow metadata, not purge-and-recreate it.

## Non-Goals

- Down-migrations / rollback (roll forward with a new migration).
- Peer-to-peer migration propagation.
- Arbitrary cross-dialect translation for edge cases — `CUSTOM` remains the escape hatch.

---

## 1. Core Model

### 1.1 The chain

A migration is a directed edge between two schema states:

```
hash_0  ──m1──▶  hash_1  ──m2──▶  hash_2  ──m3──▶  hash_3  ═ current
```

Each migration row carries the `from_hash` and `to_hash` it bridges. The server's "current schema" is identified by the last `to_hash` in the chain. Clients always know their own local `schema_hash`; the server resolves "which migrations do you need?" by walking the chain from the client's hash forward.

This structure gives us three things for free:

- **Optimistic concurrency on push.** A push is only accepted if `from_hash = server_current_hash`. Two developers pushing concurrently → second one fails with a clear "schema moved; pull and rebase" error.
- **Delta fetches.** Client sends `current_schema_hash = X`; server returns the ordered tail of the chain starting at X.
- **No fork ambiguity.** If a client presents a hash that isn't on the server's chain, that is a detectable terminal state (not silent divergence). See §9 for the recovery flow.

### 1.2 The `cloudsync_migrations` table

Stored on both server and every client. Server is the writer of new rows; clients mirror the rows they have applied.

```sql
CREATE TABLE cloudsync_migrations (
    version        INTEGER PRIMARY KEY,   -- monotonic, server-assigned
    from_hash      TEXT    NOT NULL,      -- schema_hash this migration applies to
    to_hash        TEXT    NOT NULL,      -- schema_hash produced by this migration
    op_kind        TEXT    NOT NULL,      -- 'ddl' | 'init_sync' | 'cleanup' | 'baseline'
    target_table   TEXT,                  -- affected table (NULL for multi-table DDL)
    ddl_sqlite     TEXT,                  -- native SQLite DDL
    ddl_postgresql TEXT,                  -- native PostgreSQL DDL
    metadata       TEXT,                  -- JSON: op-specific hints (rename mapping, algo, filter, …)
    checksum       TEXT    NOT NULL,      -- integrity check over the row
    applied_at     INTEGER,               -- NULL until applied locally
    last_error     TEXT                   -- diagnostic on failure, NULL otherwise
);
```

`ddl_sqlite` and `ddl_postgresql` are both populated. The developer provides one; the HTTP service translates to the other (see §4). Each client executes only the column matching its backend.

`metadata` carries the small amount of structured information that can't be recovered from the raw DDL without a full parse — most importantly:

- For `rename_column`: `{"rename": {"table": "tasks", "old": "foo", "new": "bar"}}` — consumed by the alter-lifecycle rename fix (§6).
- For `init_sync`: `{"algo": "cls", "filter": "user_id = current_user()"}`.
- For `create_table`: `{"columns": [...], "primary_key": [...]}` — used for the idempotency check when a client already has the table (§8).

The extension parses the DDL on the **service** side (where heavyweight parsers can live) and stamps this metadata at push time. The on-device code reads it but never re-parses.

### 1.3 `schema_hash` scope

The existing `schema_hash` (FNV-1a over tracked tables) stays as-is. The migration chain uses the same hash. On `commit_alter` the client recomputes the hash from the new schema; the result must equal `to_hash` of the just-applied migration, otherwise the migration is considered failed (integrity check).

---

## 2. SQL Surface

Everything a developer or client runtime needs is a SQLite scalar or table-valued function, in the same style as the rest of the extension. **There is no public C API for the migration system.** C call sites that exist today (internal callers in `src/migration.c`, `src/sqlite/migration_sqlite.c`, `src/postgresql/migration_postgresql.c`) are implementation details of the SQL functions and are not part of the developer contract.

Consequences of this rule:

- The migration system is callable from any environment that can open a SQLite connection with the extension loaded — SQLiteCloud SQL consoles, psql (once the PG-side functions are registered), mobile drivers (Kotlin, Swift, Dart, React Native), server-side drivers (Node, Python, Go), testing harnesses, CI pipelines. No custom binding layer needed per language.
- Authoring tools and IDEs can script migrations the same way they script any other SQL.
- Debugging is tractable: a failed migration's state is inspectable with a `SELECT` against `cloudsync_migrations`, not a C struct dump.

### 2.1 Authoring (privileged — requires `schema:write` scope on the apikey)

```sql
-- Developer writes native DDL in whichever dialect they're using locally.
-- The extension captures it, pushes to the server, and the server
-- auto-translates to the other dialect.
SELECT cloudsync_migration_push(
  :ddl_text,               -- e.g. 'ALTER TABLE tasks ADD COLUMN priority INTEGER DEFAULT 0'
  :dialect                 -- 'sqlite' | 'postgresql' (optional; inferred from backend)
);

-- Manual translation override: the developer provides both dialects
-- explicitly. The service skips auto-translation and stores the strings
-- verbatim. Use this when auto-conversion would be wrong (e.g. PG JSONB →
-- SQLite TEXT with a different semantic), when a statement has no
-- automatic equivalent, or when the developer wants to guarantee exactly
-- which SQL runs on each backend.
SELECT cloudsync_migration_push(
  :ddl_sqlite,             -- SQL to run on SQLite-backed participants
  :ddl_postgresql          -- SQL to run on PostgreSQL-backed participants
);

-- Partial override: auto-translate for one side, pin the other side.
-- Either argument may be NULL to mean "auto-generate this dialect".
SELECT cloudsync_migration_push(
  :ddl_sqlite,             -- verbatim
  NULL                     -- → service auto-translates from ddl_sqlite
);

-- Recording a cloudsync_init call as a migration (happens implicitly when
-- cloudsync_init is invoked during an authoring session; explicit form for
-- scripted authoring):
SELECT cloudsync_migration_push_init('tasks', 'cls', :filter);
SELECT cloudsync_migration_push_cleanup('tasks');

-- One-shot: snapshot the current database schema as the baseline (version 1,
-- from_hash = zero, to_hash = current). Safe to call exactly once per
-- deployment, and called again after an out-of-band drift event (see §9).
SELECT cloudsync_migration_baseline();
```

The `push` function is overloaded by argument count and type. One argument = auto-translate from the single provided dialect. Two non-NULL arguments = manual translation for both sides. Two arguments with one NULL = pin the non-NULL side, auto-translate the other.

### 2.2 Consumption (unprivileged — any client with sync access)

```sql
-- Normally automatic: the sync layer calls this when it receives a DML
-- payload whose schema_hash it doesn't recognize. Exposed explicitly for
-- tooling and tests.
SELECT cloudsync_migration_catchup();    -- fetches + applies delta from server

-- Inspection
SELECT * FROM cloudsync_migrations_pending;
SELECT * FROM cloudsync_migrations_applied;
```

### 2.3 Conflict resolution (privileged)

```sql
-- When apply fails because a local table diverges from the incoming DDL
-- (see §8), the developer picks a resolution strategy and retries.
SELECT cloudsync_migration_resolve(:version, :strategy);
-- strategies: 'retry' | 'skip' | 'adopt_local' | 'force_drop_recreate'
```

### 2.4 Privileged vs. unprivileged separation

Every `push` / `baseline` / `resolve` function checks the apikey scope bound to the current `cloudsync_context`. Client apps shipped to end users use a `sync:read+write` apikey which rejects authoring calls. Developer tools use a `schema:write` apikey. The scope check happens in the extension, not only on the server, so a misconfigured app cannot even attempt a push.

---

## 3. Workflows

### 3.1 Developer authors directly on the server

1. Developer connects a SQL tool (SQLiteCloud CLI, psql) to the cloud DB with a `schema:write` apikey.
2. Runs DDL, e.g. `ALTER TABLE tasks ADD COLUMN priority INTEGER DEFAULT 0`.
3. Server executes the DDL, parses it, computes `to_hash`, inserts the row into `cloudsync_migrations`, translates to the other dialect, and emits a "schema version N+1 available" broadcast on all open sync connections.
4. Connected clients pick up the broadcast at the next sync tick and run §3.3.

### 3.2 Developer authors locally, pushes to the server

1. Developer opens a local SQLite DB with the extension loaded and a `schema:write` apikey.
2. Runs DDL locally — **not** through `cloudsync_migration_push` yet; just native `ALTER TABLE …`. The alter lifecycle runs, local `schema_hash` advances. Nothing leaves the device.
3. When satisfied, developer calls `cloudsync_migration_push(ddl_text, 'sqlite')`. The extension validates that the resulting local state matches `to_hash` implied by the DDL, ships it to the server. Server assigns version, translates, stores, broadcasts.
4. `cloudsync_init('tasks')` calls made during the local session are captured via `cloudsync_migration_push_init` — either automatically (if the context is in "authoring" mode, toggled by an explicit `cloudsync_migration_begin_session()` / `_end_session()` pair) or by explicit call. The authoring-session approach is preferable because it lets the developer iterate freely and push a coherent batch at the end.

### 3.3 End-user client — automatic catch-up

1. Client sends DML payload with `schema_hash = X`.
2. Server sees `X ≠ current_hash`, rejects with a structured error:
   `{"error": "schema_mismatch", "client_hash": "X", "server_hash": "Y"}`.
3. Client (inside the network layer, not application code) calls `cloudsync_migration_catchup()`.
4. `catchup()` sends `GET /v1/migrations?from=X` to the service → ordered list of migrations from X to Y.
5. For each row in order:
   a. Verify `checksum`.
   b. Verify `from_hash` equals current local `schema_hash` (chain integrity).
   c. If `op_kind = 'init_sync'`: call `cloudsync_init_table(table, algo, filter)`.
   d. If `op_kind = 'cleanup'`: call `cloudsync_cleanup(table)`.
   e. If `op_kind = 'ddl'` or `'baseline'`: wrap in `cloudsync_begin_alter(target_table)` + execute `ddl_sqlite` + `cloudsync_commit_alter(target_table)`.
   f. Verify new `schema_hash = to_hash`.
   g. `UPDATE cloudsync_migrations SET applied_at = <now> WHERE version = ?`.
6. Client retries the DML payload. Now `schema_hash` matches and the sync proceeds.

### 3.4 Cold-start — brand-new client, empty database

Same flow as §3.3 but the client's initial `schema_hash` is the zero hash. The server returns the full chain starting from `from_hash = zero`, which begins with the `baseline` migration (see §7) and then walks every subsequent migration. The client arrives at `current_hash` without any application-side schema code.

### 3.5 Retrofit — existing deployment turning the feature on

1. Operator runs `SELECT cloudsync_migration_baseline()` **once** on the cloud DB.
2. The server introspects its current schema, emits a single migration row with `version=1, from_hash=zero, to_hash=current, op_kind='baseline'`, and populates `ddl_sqlite` / `ddl_postgresql` with the CREATE TABLE + CREATE INDEX + cloudsync_init statements that reproduce the current state.
3. Existing clients, on their next sync, see schema_mismatch and go through §3.3. Their local `schema_hash` already equals `to_hash` (they have the schema), so `catchup()` short-circuits: it records version 1 as `applied_at = <now>` without executing the DDL, because the hash check confirms there's nothing to do. This is the §8.1 idempotency path.
4. New clients see an empty DB, execute the baseline migration, and arrive at the same state.

No end-user action required. No version-zero replay across years of history. The migration log starts when the operator says it starts.

---

## 4. Dialect Translation

### 4.1 Where it happens

**On the CloudSync HTTP service**, not in the extension. Reasons:

- The extension is already split into two backends (`src/sqlite` and `src/postgresql`) that each emit their own dialect. Teaching either of them to parse and translate the *other* dialect is a large new responsibility.
- Translation benefits from heavyweight parsers (`sqlglot`, `pg_query`, `libpg_query`) that are awkward to ship inside the extension binary.
- A single server-side implementation keeps both stored dialects consistent — clients never translate, they just read the column for their backend.

### 4.2 When it happens

At `cloudsync_migration_push` time. The service receives `(ddl_text, dialect)`, translates to the other dialect, stores both, broadcasts. By the time a client sees the row, the column it needs is already populated.

### 4.3 Manual translation override and fallback

Auto-translation is best-effort. Three cases require manual intervention, all handled by the two-argument form of `cloudsync_migration_push` (§2.1):

1. **Automatic translation fails** — the service cannot parse the statement or cannot map a construct to the other dialect. The `push` call with a single dialect returns a translation error identifying the offending fragment; the developer re-pushes with both sides provided explicitly.
2. **Automatic translation would be semantically wrong.** Example: PG `JSONB` auto-translates to SQLite `TEXT`, which preserves the data but loses the indexing and operator semantics. If the developer wants SQLite to use a different representation (a separate key-value table, a virtual FTS5 table, etc.) they provide the SQLite side manually while keeping the PG side auto-translated or also manual.
3. **Developer wants explicit control.** For sensitive operations — primary-key changes, destructive column operations, anything touching a large table where the generated SQL's performance profile matters — the developer may prefer to spell both sides themselves rather than trust a generator.

The override is not a separate function ("custom migration") but the normal `push` called with two SQL strings. This is deliberate: there is one authoring primitive, not two. The service handles auto-translation and manual translation uniformly — it either accepts the developer's strings verbatim or generates the missing ones — and both paths produce identical `cloudsync_migrations` rows downstream. Clients never see a difference between "this migration was auto-translated" and "this migration was manually pinned."

The `metadata` column on the migration row records which side was manual vs. auto-generated, for audit / debugging only:

```json
{"translation": {"sqlite": "manual", "postgresql": "auto"}}
```

This lets operators identify which migrations carry developer-authored rather than tool-generated SQL — useful when diagnosing a faulty migration after the fact.

---

## 5. Parsing and Op Classification

The service parses each incoming DDL to populate `op_kind`, `target_table`, and `metadata`. This is needed because:

- The alter lifecycle must know **which table** is affected to scope `begin_alter`/`commit_alter`.
- Rename-column needs old/new names extracted into `metadata` so the client can update shadow metadata (§6).
- Create-table needs the column list extracted for the idempotency check on clients that already have the table (§8.1).

Parsing lives on the service, never on-device. If parsing fails (unrecognized dialect extension), the service stores `op_kind = 'ddl'` with an empty metadata object and the DDL still executes — the client just falls back to the conservative "wrap everything in begin_alter/commit_alter on an inferred best-effort target table, or error out if ambiguous" path.

---

## 6. Alter Lifecycle Changes

### 6.1 Rename-aware metadata translation

The current `cloudsync_finalize_alter` in `src/cloudsync.c:2497` is schema-diff based: `sql_build_delete_cols_not_in_schema_query` at line 2539 purges shadow rows whose `col_name` is not in the new source schema. For RENAME that deletes all CRDT history for the renamed column.

**Change:** `cloudsync_begin_alter` grows an optional rename hint:

```c
int cloudsync_begin_alter_rename(cloudsync_context *data,
                                 const char *table_name,
                                 const char *old_col,
                                 const char *new_col);
```

And the SQL surface mirrors it:

```sql
SELECT cloudsync_begin_alter('tasks', 'old_col', 'new_col');  -- 3-arg form
```

`cloudsync_finalize_alter` consumes the hint: before it runs the schema-diff purge, it executes

```sql
UPDATE {table}_cloudsync SET col_name = :new WHERE col_name = :old;
```

The subsequent diff finds nothing to purge (both old and new names are accounted for) and the CRDT history carries forward. For ADD/DROP (no rename hint) the existing behavior is unchanged.

The migration catchup loop calls the 3-arg form when `op_kind='ddl'` and `metadata.rename` is present; otherwise the 1-arg form.

### 6.2 Multi-statement migrations

A baseline migration may contain many `CREATE TABLE` / `CREATE INDEX` / `cloudsync_init` statements. These are wrapped in a single savepoint but each statement is executed through its own `begin_alter` / `commit_alter` pair (or directly, for statements that don't touch tracked tables). Failure anywhere rolls back the whole savepoint and leaves `applied_at = NULL`, `last_error = '…'`.

---

## 7. Enrollment as a First-Class Migration

`cloudsync_init` and `cloudsync_cleanup` are part of schema state. A client whose `schema_hash` is current must also be enrolled in the same tables with the same algorithms and filters as every other client. Therefore:

- Every `cloudsync_init(table, algo, filter)` call issued by a developer during an authoring session emits an `op_kind='init_sync'` migration row.
- Every `cloudsync_cleanup(table)` call emits an `op_kind='cleanup'` row.
- The `schema_hash` calculation incorporates the set of enrolled tables and their algos, so an enroll / un-enroll change produces a new `to_hash` and is therefore visible to the chain integrity check.

For baseline migrations (§3.5), the `CREATE TABLE` statements are followed by synthesized `cloudsync_init` calls in the same migration, reflecting the current enrollment state of the source DB.

---

## 8. Conflict Handling on Apply

When `catchup()` executes a migration whose DDL would collide with existing local state, the outcome depends on whether the local state is *compatible* with the DDL.

### 8.1 Compatible — silent idempotency

For `CREATE TABLE` and `CREATE INDEX`, the client computes a structural fingerprint of the local object (column names, types, nullability, defaults, PK, index columns) and compares to the descriptor in `metadata`. If identical:

- Skip the DDL execution.
- Mark `applied_at = <now>`.
- Move on.

This is what makes §3.5 work: existing clients don't re-run the baseline DDL they already have.

### 8.2 Divergent — fail and surface

If the fingerprints differ, `catchup()` halts at that version with a structured error:

```json
{
  "error": "migration_conflict",
  "version": 12,
  "table": "tasks",
  "detail": "local column 'priority' is TEXT, migration expects INTEGER",
  "local_fingerprint": "...",
  "expected_fingerprint": "..."
}
```

`applied_at` stays NULL, `last_error` is populated, the DML retry does not happen, and the application receives the error. The developer then uses `cloudsync_migration_resolve(version, strategy)`:

- **`retry`** — no-op; just attempts apply again. Useful if the developer manually fixed the local schema.
- **`skip`** — marks applied without executing. Dangerous; only for post-hoc recovery.
- **`adopt_local`** — treats local as correct, skips DDL, marks applied. Acceptable when the fingerprints differ only in ways the developer judges irrelevant (e.g. a column comment, a check expression that is semantically equivalent).
- **`force_drop_recreate`** — drops the local table and re-runs the DDL. Loses local data for that table. Reserved for developer workstations, not production clients.

End-user apps never choose a strategy. The app surfaces the error; the developer triages via support tooling. The privileged apikey requirement on `resolve` enforces this.

### 8.3 Missing table for ALTER

Symmetric case: `ALTER TABLE tasks …` runs against a client where `tasks` doesn't exist. This should not happen on a well-formed chain (any `ALTER` is preceded by a `CREATE` in the same chain), but defensively the client returns `migration_conflict` with `detail = "table 'tasks' not found locally"`. Resolution strategies are the same.

---

## 9. Out-of-Band Schema Changes and Chain Recovery

The migration chain is only complete if every schema change goes through `cloudsync_begin_alter` / `cloudsync_commit_alter` (directly, or via `cloudsync_migration_push`). When a developer bypasses the lifecycle and mutates the server schema directly — with psql, the SQLiteCloud CLI, a manual `ALTER TABLE`, a pgAdmin GUI — the extension has no opportunity to observe the DDL. The chain is now *silent*: the server's actual schema no longer matches `to_hash` of the last recorded migration, and the DDL itself is unrecoverable because no one captured the statement text.

This section defines detection and recovery. The core property we accept: **once a server has drifted out-of-band, there is no way to reconstruct the lost migration**. Recovery is re-baselining, not backfilling.

### 9.1 Detection on the server

On every DML request (and on a periodic background check), the server recomputes the actual schema_hash from `information_schema` / `sqlite_schema` and compares it to the chain tail's `to_hash`:

- **Match** — nothing to do.
- **Mismatch** — out-of-band drift detected. The server marks its internal state as `drifted` and refuses to accept pushes or serve migrations until an operator re-baselines.

A drifted state is reported through the service's admin API and logs so operators can act on it quickly. The drift flag is per-database, not per-connection.

### 9.2 Operator recovery: cleanup + re-init + baseline

When drift is detected, the developer has no recording of the intermediate DDL, so the only honest recovery is to declare a new starting point:

1. **Cleanup existing CRDT infrastructure.** The `{table}_cloudsync` shadow tables, triggers, and settings are stale — they were built for the schema that existed before the out-of-band change. Run `cloudsync_cleanup` for every affected table (or `cloudsync_cleanup_all`). This drops shadow tables and triggers.
2. **Re-init against the new actual schema.** Call `cloudsync_init(table, algo, filter)` for each table that should remain enrolled, using the current (drifted) column set. This rebuilds shadow tables and triggers against the new reality.
3. **Call `cloudsync_migration_baseline()`.** The baseline function re-runs the §3.5 procedure: it introspects the current schema, generates CREATE TABLE + CREATE INDEX + cloudsync_init DDL, and writes a new migration row. Its `from_hash` is the zero hash (the chain effectively restarts); its `to_hash` is the new current hash. Previous migration rows are retained for audit but marked `superseded_by = <new_baseline_version>` in a dedicated column.

After step 3 the server's `drifted` flag is cleared and the chain has a well-defined tail again. Further pushes proceed normally.

**CRDT history for all synced tables is lost at re-baseline.** This is unavoidable — the shadow tables were dropped, and there is no meaningful way to merge CRDT state across an undefined schema transition. The operator should understand this as the cost of bypassing the alter lifecycle.

### 9.3 Client-side recovery: reset and resync

A client that presents a `schema_hash = X` which no longer appears anywhere on the server's chain (because the chain was reset at re-baseline) is in a terminal state. There is no delta the server can send that would get it from X to current, because the intermediate DDL was never captured. The client's local CRDT metadata is also tied to a schema state that no longer exists on the server.

The only correct recovery is:

1. **Wipe local CloudSync state** — drop all `{table}_cloudsync` shadow tables, `cloudsync_settings`, `cloudsync_site_id`, and any `cloudsync_migrations` rows.
2. **Run catchup from zero** — local `schema_hash` is now the zero hash; the server returns the full chain starting at the new baseline, which executes CREATE TABLE + cloudsync_init for every synced table.
3. **Reset sync version** — `cloudsync_network_reset_sync_version()` so the next DML sync behaves like a first connection.
4. **Full receive** — the next `cloudsync_network_sync()` pulls all current rows from the server into the freshly re-initialized local tables.

### 9.4 Protecting local user data during reset

Steps 1–4 above silently destroy the local database's synced rows *if the local user has unsent changes*. That's a data-loss event the end user must be allowed to weigh in on.

The client detects the terminal state — the server returns `{"error": "chain_reset", "server_baseline_version": N}` when asked for a delta from an unreachable `from_hash` — and does **not** proceed automatically. Instead it raises a structured event to the application:

```json
{
  "event": "migration_chain_reset",
  "unsent_changes": true,
  "unsent_change_count": 17,
  "last_successful_sync_at": 1730000000,
  "options": ["reset_and_resync", "stay_offline"]
}
```

The application presents this to the end user. Two outcomes:

- **`reset_and_resync`** — the user accepts the data loss. The client performs steps 1–4. Unsent changes are discarded.
- **`stay_offline`** — the client stops sync and continues operating against the stale local schema. No writes go to the server; reads still work against local data. The user can later export unsent changes manually (via application-level tooling) and then request a reset.

If `unsent_changes = false`, the client **may** proceed with reset automatically (no user consent needed, nothing is being lost), controlled by a per-deployment policy flag (`auto_reset_on_chain_loss`). The default is conservative: always ask the user, even when no changes would be lost, because the UX of "your local database was silently wiped" is worse than one extra confirmation dialog.

### 9.5 Preventing out-of-band drift in the first place

Re-baselining is expensive. The system should make it easy to do the right thing:

- **Server-side DDL proxy.** The service exposes a `POST /v1/ddl` endpoint that accepts a raw DDL statement, runs it through `cloudsync_migration_push` internally, and applies it. Developers using psql or the CLI should be steered toward this endpoint rather than direct `ALTER TABLE`.
- **Event-trigger-based capture (PostgreSQL).** PostgreSQL supports event triggers on DDL (`ddl_command_end`). The service can install such a trigger that records every observed DDL into `cloudsync_migrations` automatically, turning "developer forgot to use push" from a chain-reset event into a silent correct capture. This covers the PG case entirely.
- **SQLite authorizer hook.** On the SQLite-backed side, the extension can install a `sqlite3_set_authorizer` callback that vetoes any DDL outside an active `cloudsync_begin_alter` / `cloudsync_commit_alter` window. This is stricter (outright blocks bypass) but prevents drift proactively.

Both hooks reduce the frequency of §9.2 / §9.3 from "happens whenever a developer forgets" to "happens only in genuinely unrecoverable situations (direct filesystem edits, restore from backup of a divergent DB, etc.)".

---

## 10. Failure Recovery

- Each migration applied inside a savepoint. Failure → rollback, `applied_at` stays NULL, `last_error` populated, `catchup()` halts at that version.
- Next `catchup()` automatically retries from the halted version. Idempotent for DDL that succeeded on the prior attempt (the savepoint rolled it back); no state corruption.
- Transient errors (network, lock contention) resolve on retry with no developer action. Structural errors land in the `resolve` flow (§8.2).
- Chain-loss errors (§9) are distinct from per-migration failures — they signal "no path forward exists," not "this particular migration failed."

---

## 11. Security

- **Apikey scopes.** `schema:write` is required for any `push` / `baseline` / `resolve` function. `sync:read+write` is sufficient for `catchup`. End-user clients are never issued `schema:write`.
- **Server-side authorization.** The scope check is enforced on the server at push time, regardless of what the client claims. The extension's check is defense-in-depth.
- **Checksum.** FNV-1a over the full migration row (including both DDL columns and metadata). Detects transport corruption, not a hostile transport; TLS provides the confidentiality / authenticity layer.
- **Broadcast privilege.** Only the service emits "schema version N available" frames; clients cannot forge them.

---

## 12. Relationship to Existing Code

**Reused as-is:**

- `schema_hash` FNV-1a calculation.
- `cloudsync_begin_alter` / `cloudsync_commit_alter` lifecycle (with the rename-aware addition in §6.1).
- Network payload schema_hash gate — it's already the trigger for §3.3 catch-up.
- The split between `src/sqlite/` and `src/postgresql/` — each backend just executes native DDL; neither needs to know about the other dialect.

**New:**

- `src/migration.c` is rewritten around native SQL rather than binary descriptors. The op table shrinks from 11 operations to a small classifier (`ddl` / `init_sync` / `cleanup` / `baseline`).
- `cloudsync_finalize_alter` gains a rename path.
- SQL surface (all functions are the public API — there is no corresponding C API): `cloudsync_migration_push` (1-arg and 2-arg overloads, the 2-arg form being the manual-translation override), `cloudsync_migration_push_init`, `cloudsync_migration_push_cleanup`, `cloudsync_migration_baseline`, `cloudsync_migration_catchup`, `cloudsync_migration_resolve`. Inspection views / table-valued functions: `cloudsync_migrations_pending`, `cloudsync_migrations_applied`.
- Service-side: DDL parsing, cross-dialect translation, `GET /v1/migrations`, `POST /v1/migrations`, `POST /v1/ddl`, broadcast of schema-version events, drift detection.
- Optional DDL-capture hooks: PostgreSQL event trigger, SQLite authorizer callback.

**Removed:**

- The C-only descriptor API and binary serialization (`cloudsync_migration_create`, setters, `_serialize`, `_deserialize`). The `CSTYPE_*` enum and the `cloudsync_migration_descriptor` struct go with it.
- The abstract type-mapping table in `docs/MIGRATION.md`. Native DDL makes the mapping the database's problem, not ours.

---

## 13. Open Questions

1. **Authoring sessions.** Is the "implicit capture between `begin_session` / `end_session`" model the right ergonomics, or should every local DDL statement be an explicit push? The batch model allows iteration but risks the developer forgetting to end the session. A middle ground: capture implicitly but require an explicit `push_session` call to publish — `end_session` without `push_session` discards.

2. **Index naming on cross-dialect translation.** PostgreSQL and SQLite have slightly different auto-generated index names for unnamed constraints. The translator must force explicit names to keep idempotency fingerprints consistent.

3. **`INIT_SYNC` re-enrollment.** Changing algo/filter on an already-enrolled table is still undefined. Proposal: model it as `cleanup` + `init_sync` within a single migration (stored as two rows? one row with a compound op?). Needs prototyping.

4. **Baseline squashing.** `cloudsync_migration_baseline()` is documented as once-per-deployment plus once-per-drift. Do we also want a `cloudsync_migration_compact()` that folds versions 1..N into a new synthetic baseline once the chain gets long? The chain model supports it — set `from_hash = zero, to_hash = current` on the new baseline row and mark 1..N as `superseded_by` — but care is needed around mid-upgrade clients whose `applied_at` points at a version that no longer exists on the live chain. Likely a v2 concern.

5. **Drift-detection cost.** Recomputing `schema_hash` from `information_schema` on every request is not free. A cheaper gate: install the event trigger / authorizer hooks (§9.5) and only recompute on a background cadence (every 60 seconds, plus at connect time). Open question is whether the hook coverage is complete enough to drop the periodic check entirely.

6. **Observability.** What diagnostic hooks does the service need to expose to tooling? At minimum: chain visualization, pending-migration count per client, failed-migration histogram, drift-event log. The `cloudsync_migrations` table already carries the raw data; the question is what views/endpoints wrap it.
