//
//  migration.c
//  cloudsync
//
//  Created by Marco Bambini on 15/04/26.
//

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "migration.h"
#include "cloudsync.h"
#include "dbutils.h"
#include "sql.h"

// MARK: - Binary serialization constants -

#define MIGR_MAGIC          0x4D494752UL  // 'MIGR'
#define MIGR_VERSION        1

// Field IDs used in the TLV payload
#define MIGFIELD_TABLE        0x01
#define MIGFIELD_NEW_NAME     0x02
#define MIGFIELD_COL_NAME     0x03
#define MIGFIELD_COL_TYPE     0x04
#define MIGFIELD_COL_NULLABLE 0x05
#define MIGFIELD_COL_DEFAULT  0x06
#define MIGFIELD_COL_HAS_DEF  0x07
#define MIGFIELD_COLUMNS      0x08
#define MIGFIELD_INDEX_NAME   0x09
#define MIGFIELD_INDEX_COLS   0x0A
#define MIGFIELD_INDEX_UNIQUE 0x0B
#define MIGFIELD_ALGO         0x0C
#define MIGFIELD_FILTER       0x0D
#define MIGFIELD_SQL_SQLITE   0x0E
#define MIGFIELD_SQL_PGSQL    0x0F

// Operations that require a cloudsync_begin_alter / cloudsync_commit_alter wrap.
// CUSTOM is included because it is the documented escape hatch for unsupported
// schema changes (e.g. SET DEFAULT on SQLite, type/constraint changes on PG).
// When desc->table is set for a CUSTOM migration the caller wraps the DDL in
// begin_alter/commit_alter so triggers and shadow metadata stay in sync.
// When desc->table is NULL the alter-lifecycle guard in the apply loop is a no-op.
static bool migration_needs_alter_lifecycle(cloudsync_migration_op op) {
    switch (op) {
        case CLOUDSYNC_MIGRATION_ADD_COLUMN:
        case CLOUDSYNC_MIGRATION_DROP_COLUMN:
        case CLOUDSYNC_MIGRATION_RENAME_COLUMN:
        case CLOUDSYNC_MIGRATION_SET_DEFAULT:
        case CLOUDSYNC_MIGRATION_CUSTOM:
            return true;
        default:
            return false;
    }
}

// Split a possibly schema-qualified table name ("schema.table") into its parts.
// Writes the schema into schema_buf (empty string if unqualified) and returns a
// pointer to the bare table name within name.  Never writes more than buf_size
// bytes (including the NUL terminator); on overflow the schema is left empty and
// the full name is returned unchanged so callers degrade gracefully.
static const char *migration_split_name(const char *name,
                                         char *schema_buf, size_t buf_size) {
    const char *dot = (name && schema_buf && buf_size > 0) ? strchr(name, '.') : NULL;
    if (dot) {
        size_t slen = (size_t)(dot - name);
        if (slen > 0 && slen < buf_size) {
            memcpy(schema_buf, name, slen);
            schema_buf[slen] = '\0';
            return dot + 1;
        }
    }
    if (schema_buf && buf_size > 0) schema_buf[0] = '\0';
    return name;
}

// Apply a DROP_TABLE migration, keeping CloudSync metadata consistent.
// Cleans up the shadow table, triggers, and settings rows before the physical
// table is dropped so that no orphaned metadata remains after the DDL.
// Works even when the table is not currently loaded into the context.
static int migration_apply_drop_table(cloudsync_context *ctx,
                                       cloudsync_migration_descriptor *desc) {
    const char *name = desc->table;

    // Split any explicit schema prefix first.  An "schema.table" descriptor must
    // map to the bare name for all metadata operations: trigger helpers,
    // cloudsync_table_settings rows, and the in-memory context entry are all keyed
    // by the bare table name used at cloudsync_init time.
    char schema_buf[256] = {0};
    const char *tname  = migration_split_name(name, schema_buf, sizeof(schema_buf));
    const char *schema = schema_buf[0] ? schema_buf : cloudsync_schema(ctx);

    // For schema-qualified descriptors, temporarily redirect the context schema so
    // that trigger helpers (which resolve schema via cloudsync_schema) target the
    // right schema instead of the search-path default.
    // Copy the current schema into a stack buffer BEFORE calling cloudsync_set_schema:
    // that function frees data->current_schema, so any raw pointer returned by
    // cloudsync_schema() becomes dangling the moment the switch fires.
    char saved_schema_buf[256] = {0};
    {
        const char *_s = cloudsync_schema(ctx);
        if (_s) snprintf(saved_schema_buf, sizeof(saved_schema_buf), "%s", _s);
    }
    const char *saved_schema = saved_schema_buf[0] ? saved_schema_buf : NULL;
    bool schema_switched = (schema_buf[0] != '\0');
    if (schema_switched) cloudsync_set_schema(ctx, schema_buf);

    // For schema-qualified descriptors, verify the table is actually tracked in
    // this schema before touching any CloudSync metadata.  Skipping cleanup is
    // correct when:
    //   (a) the in-memory entry has a different (non-NULL) schema — positive
    //       mismatch: "public.orders" is synced but the descriptor drops "sales.orders";
    //   (b) no in-memory entry exists AND a schema qualifier is present — we cannot
    //       verify which schema the table belongs to, so we are conservative: better
    //       to leave a stale settings row than to delete the settings for a correctly-
    //       enrolled same-named table in a different schema.
    // Unqualified drops (no schema_buf) always perform full CloudSync cleanup,
    // which preserves the original behavior for non-schema-qualified deployments.
    if (schema_buf[0]) {
        bool skip_cloudsync_cleanup = false;
        cloudsync_table_context *entry = table_lookup(ctx, tname);
        if (!entry) {
            // Not in context with a schema qualifier — cannot verify schema.
            skip_cloudsync_cleanup = true;
        } else {
            const char *entry_schema = table_get_schema(entry);
            // NULL / empty entry_schema means the table was enrolled without an
            // explicit schema context and matches any qualifier (backward compat).
            if (entry_schema && entry_schema[0] &&
                strcasecmp(entry_schema, schema_buf) != 0) {
                skip_cloudsync_cleanup = true;  // positive schema mismatch
            }
        }
        if (skip_cloudsync_cleanup) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return database_migration_execute(ctx, desc);
        }
    }

    int rc = DBRES_OK;

    // Drop triggers.  IF EXISTS makes this a no-op for untracked tables.
    // Any genuine DB error (OOM, corruption) aborts before the physical DROP.
    rc = database_delete_triggers(ctx, tname);
    if (rc != DBRES_OK) {
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Drop shadow table.  IF EXISTS → no-op if never synced; abort on real error.
    char *meta_ref = database_build_meta_ref(schema, tname);
    if (meta_ref) {
        char *shadow_sql = dbmem_mprintf("DROP TABLE IF EXISTS %s;", meta_ref);
        if (shadow_sql) {
            rc = database_exec(ctx, shadow_sql);
            dbmem_free(shadow_sql);
        }
        dbmem_free(meta_ref);
        if (rc != DBRES_OK) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return rc;
        }
    }

    // Drop blocks table.  IF EXISTS makes this a no-op for non-block-LWW tables
    // (where the blocks table was never created), so any returned error is a
    // genuine DB failure (permissions, disk, corruption).  Treat it as fatal:
    // leaving stale block state while the user table and settings are gone
    // would corrupt a subsequent re-enroll of the same table name.
    char *blocks_ref = database_build_blocks_ref(schema, tname);
    if (blocks_ref) {
        char *blocks_sql = dbmem_mprintf("DROP TABLE IF EXISTS %s;", blocks_ref);
        if (blocks_sql) {
            rc = database_exec(ctx, blocks_sql);
            dbmem_free(blocks_sql);
        }
        dbmem_free(blocks_ref);
        if (rc != DBRES_OK) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return rc;
        }
    }

    // Remove per-table settings rows.  Abort on failure to avoid dropping the
    // user table while cloudsync_table_settings still has stale rows for it.
    {
        const char *vals[] = {tname};
        DBTYPE types[] = {DBTYPE_TEXT};
        int lens[] = {-1};
        rc = database_write(ctx, SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE, vals, types, lens, 1);
        if (rc != DBRES_OK) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return rc;
        }
    }

    // Evict from the in-memory context.  The global last-table epilogue
    // (cloudsync_reset_siteid + dbutils_settings_cleanup) is intentionally
    // deferred to apply_pending after the full batch commits.  Running it here
    // would break batches that temporarily reach zero tracked tables mid-batch
    // and then INIT_SYNC a new table later: the later INIT_SYNC would receive a
    // fresh site_id, causing the same client to commit the batch under a
    // different replica identity than all prior sync history.
    cloudsync_forget_table(ctx, tname);

    if (schema_switched) cloudsync_set_schema(ctx, saved_schema);

    // Execute the physical DDL: DROP TABLE IF EXISTS <name>
    return database_migration_execute(ctx, desc);
}

// Callback invoked once per block column when reloading block settings after a
// rename.  Reads the optional delimiter from cloudsync_table_settings and wires
// the column into the in-memory context via cloudsync_setup_block_column.
typedef struct { cloudsync_context *ctx; const char *table_name; } migration_block_cb_t;

static int migration_reload_block_col_cb(void *xdata, int ncols, char **values, char **names) {
    (void)ncols; (void)names;
    migration_block_cb_t *bc = (migration_block_cb_t *)xdata;
    const char *col_name = values[0];
    if (!col_name) return 0;

    char dbuf[256] = {0};
    int drc = dbutils_table_settings_get_value(bc->ctx, bc->table_name,
                                                col_name, "delimiter",
                                                dbuf, sizeof(dbuf));
    const char *delim = (drc == DBRES_OK && dbuf[0]) ? dbuf : NULL;
    cloudsync_setup_block_column(bc->ctx, bc->table_name, col_name, delim, false);
    return 0;
}

// Apply a RENAME_TABLE migration, keeping CloudSync metadata consistent:
// triggers are dropped, main table renamed, shadow table renamed, settings
// tbl_name updated, and triggers recreated under the new name.
// For local-only tables (never enrolled via INIT_SYNC) only the DDL rename is
// executed — no shadow/trigger/settings manipulation is performed and the table
// is not enrolled in CloudSync as a side-effect.
static int migration_apply_rename_table(cloudsync_context *ctx,
                                         cloudsync_migration_descriptor *desc) {
    const char *old_name = desc->table;
    const char *new_name = desc->new_name;

    // Extract schema prefix early — before the tracked-table check — so we can
    // try both the qualified form ("sales.orders") and the bare form ("orders")
    // when looking up the in-memory entry.  The registry stores bare names when
    // the table was enrolled via cloudsync_set_schema('sales') + init('orders').
    char schema_buf[256] = {0};
    const char *old_tname = migration_split_name(old_name, schema_buf, sizeof(schema_buf));
    char new_schema_buf[256] = {0};
    const char *new_tname = migration_split_name(new_name, new_schema_buf, sizeof(new_schema_buf));
    // Effective schema for shadow/blocks ref builders (handles context fallback).
    const char *schema = schema_buf[0] ? schema_buf : cloudsync_schema(ctx);

    // If the table is not currently tracked by CloudSync it is local-only.
    // In that case only the DDL rename is needed; there are no shadow tables,
    // triggers, or settings to update, and we must NOT call cloudsync_init_table
    // for new_name because that would silently enroll a local-only table.
    // Determine whether old_name is tracked by CloudSync.  The registry stores
    // bare names, so try the full name first (handles unqualified descriptors)
    // and fall back to the bare name when a schema prefix is present.
    // When a schema prefix IS present, also require the registry entry's schema
    // to match — the same schema-aware rule applied in the apply_pending loop's
    // is_tracked check.  A NULL/empty registry schema matches any qualifier
    // (enrolled without explicit schema context).
    cloudsync_table_context *old_entry = table_lookup(ctx, old_name);
    if (!old_entry && schema_buf[0]) old_entry = table_lookup(ctx, old_tname);
    bool was_tracked = false;
    if (old_entry) {
        if (!schema_buf[0]) {
            was_tracked = true;  // unqualified descriptor — name-only match
        } else {
            const char *entry_schema = table_get_schema(old_entry);
            was_tracked = (entry_schema == NULL || entry_schema[0] == '\0' ||
                           strcasecmp(entry_schema, schema_buf) == 0);
        }
    }
    if (!was_tracked) {
        return database_migration_execute(ctx, desc);
    }

    // Read algo and filter before any cleanup so we can re-apply them after the
    // rename.  Settings are keyed by bare table name; use old_tname when the
    // descriptor carries a schema prefix so the lookup matches the stored key.
    const char *settings_key = schema_buf[0] ? old_tname : old_name;
    table_algo algo = dbutils_table_settings_get_algo(ctx, settings_key);
    if (algo == table_algo_none) algo = table_algo_crdt_cls;
    const char *algo_str = cloudsync_algo_name(algo);

    char filter_buf[2048] = {0};
    int frc = dbutils_table_settings_get_value(ctx, settings_key, "*", "filter",
                                                filter_buf, sizeof(filter_buf));
    const char *filter = (frc == DBRES_OK && filter_buf[0]) ? filter_buf : NULL;

    // When old_name carries an explicit schema prefix, temporarily switch the
    // context schema so that every metadata helper that calls cloudsync_schema()
    // internally targets the right schema (e.g. "sales") rather than the
    // current search-path schema (e.g. "public").  This makes bare-name calls
    // to database_delete_triggers, database_create_triggers, cloudsync_init_table,
    // and the block-column reload all produce correctly-qualified SQL.
    // Stack-copy the current schema before the switch: cloudsync_set_schema() frees
    // data->current_schema, so a raw pointer from cloudsync_schema() becomes
    // dangling the instant the switch executes.
    char saved_schema_buf[256] = {0};
    {
        const char *_s = cloudsync_schema(ctx);
        if (_s) snprintf(saved_schema_buf, sizeof(saved_schema_buf), "%s", _s);
    }
    const char *saved_schema = saved_schema_buf[0] ? saved_schema_buf : NULL;
    bool schema_switched = (schema_buf[0] != '\0');
    if (schema_switched) cloudsync_set_schema(ctx, schema_buf);

    // Drop triggers using the bare name; cloudsync_schema(ctx) provides the
    // schema so the helper builds the right fully-qualified table reference.
    // Abort on failure: if the old triggers cannot be removed, proceeding with
    // the rename would let cloudsync_init_table() create a second set of triggers
    // on the (now-renamed) table, leaving both sets active and causing writes to
    // be tracked twice.
    int rc = database_delete_triggers(ctx, old_tname);
    if (rc != DBRES_OK) {
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Execute the DDL: ALTER TABLE old_name RENAME TO new_name.
    // build_migration_sql handles qualified names via pgstr_append_table_ref,
    // independent of the context schema.
    rc = database_migration_execute(ctx, desc);
    if (rc != DBRES_OK) {
        // Best-effort: restore triggers on failure (bare name + context schema).
        database_create_triggers(ctx, old_tname, algo, filter);
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Rename shadow table.  The shadow MUST exist for every tracked table, so a
    // rename failure (e.g. destination already exists after a previous failed
    // cleanup) is fatal: abort and let the savepoint rollback undo the main
    // table rename that already succeeded.
    // Source is schema-qualified; destination is bare — RENAME TO never takes a
    // schema qualifier.
    char *old_meta_ref  = database_build_meta_ref(schema, old_tname);
    char *new_meta_bare = database_build_meta_ref(NULL,   new_tname);
    if (old_meta_ref && new_meta_bare) {
        char *shadow_sql = dbmem_mprintf("ALTER TABLE %s RENAME TO %s;",
                                          old_meta_ref, new_meta_bare);
        if (shadow_sql) {
            rc = database_exec(ctx, shadow_sql);
            dbmem_free(shadow_sql);
        }
    }
    dbmem_free(old_meta_ref);
    dbmem_free(new_meta_bare);
    if (rc != DBRES_OK) {
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Determine whether this table has any block-LWW columns.
    // The settings still carry old_tname as the key at this point.
    // Used below to decide whether a blocks rename failure is fatal.
    bool has_block_cols = false;
    {
        dbvm_t *bvm = NULL;
        if (databasevm_prepare(ctx, SQL_TABLE_SETTINGS_SELECT_BLOCK_COLS,
                               (void **)&bvm, 0) == DBRES_OK) {
            databasevm_bind_text(bvm, 1, old_tname, -1);
            if (databasevm_step(bvm) == DBRES_ROW) has_block_cols = true;
            databasevm_finalize(bvm);
        }
    }

    // Rename blocks table.  Fatal only when block columns are registered
    // (implying the source blocks table must exist); for non-block tables the
    // source table is absent and the failure is expected, not an error.
    // Must happen before cloudsync_cleanup so cleanup's DROP IF EXISTS is a
    // no-op instead of destroying the existing block state.
    char *old_blocks_ref  = database_build_blocks_ref(schema, old_tname);
    char *new_blocks_bare = database_build_blocks_ref(NULL,   new_tname);
    if (old_blocks_ref && new_blocks_bare) {
        char *blocks_sql = dbmem_mprintf("ALTER TABLE %s RENAME TO %s;",
                                          old_blocks_ref, new_blocks_bare);
        if (blocks_sql) {
            int brc = database_exec(ctx, blocks_sql);
            dbmem_free(blocks_sql);
            if (brc != DBRES_OK && has_block_cols) rc = brc;
        }
    }
    dbmem_free(old_blocks_ref);
    dbmem_free(new_blocks_bare);
    if (rc != DBRES_OK) {
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Update cloudsync_table_settings: move all rows from old_tname to new_tname.
    // CloudSync always stores tbl_name as the bare table name (the name used at
    // cloudsync_init time), so a schema-qualified old_name like "sales.orders"
    // would never match the stored key "orders".  Use old_tname (bare) for the
    // WHERE clause, consistent with how algo/filter lookups are keyed above.
    // Uses bound parameters (cross-platform; avoids SQLite-only %Q/%w).
    {
        const char *vals[] = {new_tname, old_tname};
        DBTYPE types[] = {DBTYPE_TEXT, DBTYPE_TEXT};
        int lens[] = {-1, -1};
        rc = database_write(ctx, SQL_TABLE_SETTINGS_RENAME_TABLE, vals, types, lens, 2);
        if (rc != DBRES_OK) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return rc;
        }
    }

    // Remove the stale in-memory entry.  The entry is also keyed by bare name,
    // so use old_tname here as well.
    cloudsync_forget_table(ctx, old_tname);

    // Re-store filter under bare new_tname; cloudsync_schema(ctx) now points to
    // the correct schema, so the helper builds the right qualified reference.
    if (filter && filter[0]) {
        rc = dbutils_table_settings_set_key_value(ctx, new_tname, "*", "filter", filter);
        if (rc != DBRES_OK) {
            if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
            return rc;
        }
    }

    // Re-init for the new bare name: creates triggers, registers in-memory context.
    // cloudsync_schema(ctx) is already pointing at the right schema, so all
    // metadata helpers called inside cloudsync_init_table target the correct
    // schema.  SKIP_INT_PK_CHECK because the table already passed at INIT_SYNC.
    rc = cloudsync_init_table(ctx, new_tname, algo_str,
                               CLOUDSYNC_INIT_FLAG_SKIP_INT_PK_CHECK);
    if (rc != DBRES_OK) {
        if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
        return rc;
    }

    // Reload block-column settings for the new name into the in-memory context.
    // cloudsync_init_table() restores table-level metadata (triggers, shadow)
    // but does not re-apply column-level block settings (algo=block rows in
    // cloudsync_table_settings).  Those rows were migrated to bare new_tname
    // above, so we query them with a bound-parameter statement.
    {
        migration_block_cb_t bc = { ctx, new_tname };
        dbvm_t *bvm = NULL;
        if (databasevm_prepare(ctx, SQL_TABLE_SETTINGS_SELECT_BLOCK_COLS,
                               (void **)&bvm, 0) == DBRES_OK) {
            databasevm_bind_text(bvm, 1, new_tname, -1);
            while (databasevm_step(bvm) == DBRES_ROW) {
                const char *col = database_column_text(bvm, 0);
                if (col) migration_reload_block_col_cb(&bc, 1,
                                                        (char **)&col, NULL);
            }
            databasevm_finalize(bvm);
        }
    }

    if (schema_switched) cloudsync_set_schema(ctx, saved_schema);
    return rc;
}

// MARK: - Write buffer helpers -

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} migbuf_t;

static int migbuf_init(migbuf_t *b, size_t initial) {
    b->data = (uint8_t *)dbmem_alloc(initial);
    if (!b->data) return DBRES_NOMEM;
    b->len = 0;
    b->cap = initial;
    return DBRES_OK;
}

static void migbuf_free(migbuf_t *b) {
    if (b->data) dbmem_free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int migbuf_grow(migbuf_t *b, size_t need) {
    if (b->len + need <= b->cap) return DBRES_OK;
    size_t new_cap = b->cap * 2;
    if (new_cap < b->len + need) new_cap = b->len + need + 64;
    uint8_t *p = (uint8_t *)dbmem_realloc(b->data, new_cap);
    if (!p) return DBRES_NOMEM;
    b->data = p;
    b->cap = new_cap;
    return DBRES_OK;
}

static int migbuf_u8(migbuf_t *b, uint8_t v) {
    if (migbuf_grow(b, 1) != DBRES_OK) return DBRES_NOMEM;
    b->data[b->len++] = v;
    return DBRES_OK;
}

static int migbuf_u16be(migbuf_t *b, uint16_t v) {
    if (migbuf_grow(b, 2) != DBRES_OK) return DBRES_NOMEM;
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    return DBRES_OK;
}

static int migbuf_u32be(migbuf_t *b, uint32_t v) {
    if (migbuf_grow(b, 4) != DBRES_OK) return DBRES_NOMEM;
    b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8)  & 0xFF);
    b->data[b->len++] = (uint8_t)(v & 0xFF);
    return DBRES_OK;
}

static int migbuf_bytes(migbuf_t *b, const void *src, size_t len) {
    if (len == 0) return DBRES_OK;
    if (migbuf_grow(b, len) != DBRES_OK) return DBRES_NOMEM;
    memcpy(b->data + b->len, src, len);
    b->len += len;
    return DBRES_OK;
}

// Write a string TLV field (omitted if str is NULL)
static int migbuf_str_field(migbuf_t *b, uint8_t fid, const char *str) {
    if (!str) return DBRES_OK;
    size_t slen = strlen(str);
    if (migbuf_u8(b, fid)                       != DBRES_OK) return DBRES_NOMEM;
    if (migbuf_u32be(b, (uint32_t)slen)          != DBRES_OK) return DBRES_NOMEM;
    if (migbuf_bytes(b, str, slen)               != DBRES_OK) return DBRES_NOMEM;
    return DBRES_OK;
}

// Write a single-byte TLV field
static int migbuf_u8_field(migbuf_t *b, uint8_t fid, uint8_t v) {
    if (migbuf_u8(b, fid)       != DBRES_OK) return DBRES_NOMEM;
    if (migbuf_u32be(b, 1)      != DBRES_OK) return DBRES_NOMEM;
    if (migbuf_u8(b, v)         != DBRES_OK) return DBRES_NOMEM;
    return DBRES_OK;
}

// MARK: - Read buffer helpers -

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} migrd_t;

static int migrd_u8(migrd_t *r, uint8_t *v) {
    if (r->pos + 1 > r->len) return DBRES_ERROR;
    *v = r->data[r->pos++];
    return DBRES_OK;
}

static int migrd_u16be(migrd_t *r, uint16_t *v) {
    if (r->pos + 2 > r->len) return DBRES_ERROR;
    *v = (uint16_t)(((uint16_t)r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return DBRES_OK;
}

static int migrd_u32be(migrd_t *r, uint32_t *v) {
    if (r->pos + 4 > r->len) return DBRES_ERROR;
    *v = ((uint32_t)r->data[r->pos]     << 24) |
         ((uint32_t)r->data[r->pos + 1] << 16) |
         ((uint32_t)r->data[r->pos + 2] <<  8) |
          (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return DBRES_OK;
}

// Read len bytes as a heap-allocated NUL-terminated string
static int migrd_string(migrd_t *r, uint32_t len, char **out) {
    if (r->pos + len > r->len) return DBRES_ERROR;
    char *s = (char *)dbmem_alloc(len + 1);
    if (!s) return DBRES_NOMEM;
    memcpy(s, r->data + r->pos, len);
    s[len] = '\0';
    r->pos += len;
    *out = s;
    return DBRES_OK;
}

// MARK: - Checksum -

uint64_t cloudsync_migration_checksum(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ULL;           // FNV-1a prime
    }
    return hash;
}

// MARK: - Names -

const char *cloudsync_migration_op_name(cloudsync_migration_op op) {
    switch (op) {
        case CLOUDSYNC_MIGRATION_ADD_COLUMN:    return "ADD_COLUMN";
        case CLOUDSYNC_MIGRATION_DROP_COLUMN:   return "DROP_COLUMN";
        case CLOUDSYNC_MIGRATION_RENAME_COLUMN: return "RENAME_COLUMN";
        case CLOUDSYNC_MIGRATION_SET_DEFAULT:   return "SET_DEFAULT";
        case CLOUDSYNC_MIGRATION_CREATE_TABLE:  return "CREATE_TABLE";
        case CLOUDSYNC_MIGRATION_DROP_TABLE:    return "DROP_TABLE";
        case CLOUDSYNC_MIGRATION_RENAME_TABLE:  return "RENAME_TABLE";
        case CLOUDSYNC_MIGRATION_CREATE_INDEX:  return "CREATE_INDEX";
        case CLOUDSYNC_MIGRATION_DROP_INDEX:    return "DROP_INDEX";
        case CLOUDSYNC_MIGRATION_INIT_SYNC:     return "INIT_SYNC";
        case CLOUDSYNC_MIGRATION_CUSTOM:        return "CUSTOM";
        default:                                return "UNKNOWN";
    }
}

const char *cloudsync_sync_algo_name(cloudsync_sync_algo algo) {
    switch (algo) {
        case CSALGO_CLS: return "cls";
        case CSALGO_GOS: return "gos";
        case CSALGO_DWS: return "dws";
        case CSALGO_AWS: return "aws";
        default:         return NULL;
    }
}

// MARK: - Constructor / destructor -

cloudsync_migration_descriptor *cloudsync_migration_create(cloudsync_migration_op op) {
    cloudsync_migration_descriptor *d =
        (cloudsync_migration_descriptor *)dbmem_zeroalloc(sizeof(*d));
    if (!d) return NULL;
    d->op = op;
    return d;
}

void cloudsync_migration_free(cloudsync_migration_descriptor *desc) {
    if (!desc) return;
    dbmem_free(desc->table);
    dbmem_free(desc->new_name);
    dbmem_free(desc->col_name);
    dbmem_free(desc->col_default);
    dbmem_free(desc->index_name);
    dbmem_free(desc->filter);
    dbmem_free(desc->sql_sqlite);
    dbmem_free(desc->sql_postgresql);

    for (int i = 0; i < desc->ncolumns; i++) {
        dbmem_free(desc->columns[i].name);
        dbmem_free(desc->columns[i].default_value);
    }
    dbmem_free(desc->columns);

    for (int i = 0; i < desc->nindex_columns; i++) {
        dbmem_free(desc->index_columns[i]);
    }
    dbmem_free(desc->index_columns);

    dbmem_free(desc);
}

// MARK: - Setters -

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)dbmem_alloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

void cloudsync_migration_set_table(cloudsync_migration_descriptor *d, const char *table) {
    if (!d) return;
    dbmem_free(d->table);
    d->table = dup_str(table);
}

void cloudsync_migration_set_new_name(cloudsync_migration_descriptor *d, const char *new_name) {
    if (!d) return;
    dbmem_free(d->new_name);
    d->new_name = dup_str(new_name);
}

void cloudsync_migration_set_column(cloudsync_migration_descriptor *d, const char *col_name) {
    if (!d) return;
    dbmem_free(d->col_name);
    d->col_name = dup_str(col_name);
}

void cloudsync_migration_set_type(cloudsync_migration_descriptor *d, cloudsync_column_type type) {
    if (!d) return;
    d->col_type = type;
}

void cloudsync_migration_set_nullable(cloudsync_migration_descriptor *d, bool nullable) {
    if (!d) return;
    d->col_nullable = nullable;
}

void cloudsync_migration_set_default(cloudsync_migration_descriptor *d, const char *default_val) {
    if (!d) return;
    dbmem_free(d->col_default);
    d->col_default     = dup_str(default_val);
    d->col_has_default = (default_val != NULL);
}

void cloudsync_migration_add_column(cloudsync_migration_descriptor *d,
                                    const char *name, cloudsync_column_type type,
                                    bool nullable, const char *default_val) {
    if (!d || !name) return;
    int n = d->ncolumns;
    cloudsync_migration_column *cols =
        (cloudsync_migration_column *)dbmem_realloc(d->columns,
            (size_t)(n + 1) * sizeof(cloudsync_migration_column));
    if (!cols) return;
    d->columns = cols;
    d->columns[n].name          = dup_str(name);
    d->columns[n].type          = type;
    d->columns[n].nullable      = nullable;
    d->columns[n].default_value = dup_str(default_val);
    d->columns[n].is_pk         = false;
    d->ncolumns++;
}

void cloudsync_migration_set_primary_key(cloudsync_migration_descriptor *d,
                                          const char *col_name) {
    if (!d || !col_name) return;
    for (int i = 0; i < d->ncolumns; i++) {
        if (d->columns[i].name && strcasecmp(d->columns[i].name, col_name) == 0) {
            d->columns[i].is_pk = true;
            return;
        }
    }
}

void cloudsync_migration_set_index_name(cloudsync_migration_descriptor *d,
                                         const char *index_name) {
    if (!d) return;
    dbmem_free(d->index_name);
    d->index_name = dup_str(index_name);
}

void cloudsync_migration_add_index_column(cloudsync_migration_descriptor *d,
                                           const char *col_name) {
    if (!d || !col_name) return;
    int n = d->nindex_columns;
    char **cols = (char **)dbmem_realloc(d->index_columns,
                                          (size_t)(n + 1) * sizeof(char *));
    if (!cols) return;
    d->index_columns = cols;
    d->index_columns[n] = dup_str(col_name);
    d->nindex_columns++;
}

void cloudsync_migration_set_index_unique(cloudsync_migration_descriptor *d, bool unique) {
    if (!d) return;
    d->index_unique = unique;
}

void cloudsync_migration_set_algo(cloudsync_migration_descriptor *d, cloudsync_sync_algo algo) {
    if (!d) return;
    d->algo = algo;
}

void cloudsync_migration_set_filter(cloudsync_migration_descriptor *d, const char *filter) {
    if (!d) return;
    dbmem_free(d->filter);
    d->filter = dup_str(filter);
}

void cloudsync_migration_set_sql_sqlite(cloudsync_migration_descriptor *d, const char *sql) {
    if (!d) return;
    dbmem_free(d->sql_sqlite);
    d->sql_sqlite = dup_str(sql);
}

void cloudsync_migration_set_sql_postgresql(cloudsync_migration_descriptor *d, const char *sql) {
    if (!d) return;
    dbmem_free(d->sql_postgresql);
    d->sql_postgresql = dup_str(sql);
}

// MARK: - Serialization -

int cloudsync_migration_serialize(const cloudsync_migration_descriptor *desc,
                                   void **out_blob, size_t *out_size) {
    if (!desc || !out_blob || !out_size) return DBRES_MISUSE;

    migbuf_t b;
    if (migbuf_init(&b, 256) != DBRES_OK) return DBRES_NOMEM;

    int rc = DBRES_NOMEM;

    // --- Header: magic (4B) + format version (1B) + op (1B) ---
    // We'll patch nfields at the end, so reserve 2 bytes for it now
    if (migbuf_u32be(&b, (uint32_t)MIGR_MAGIC) != DBRES_OK) goto fail;
    if (migbuf_u8(&b, MIGR_VERSION)             != DBRES_OK) goto fail;
    if (migbuf_u8(&b, (uint8_t)desc->op)        != DBRES_OK) goto fail;
    size_t nfields_pos = b.len;          // remember where nfields goes
    if (migbuf_u16be(&b, 0)                     != DBRES_OK) goto fail;  // placeholder

    uint16_t nfields = 0;

#define WS(fid, str)  do { if ((str)) { if (migbuf_str_field(&b, (fid), (str)) != DBRES_OK) goto fail; nfields++; } } while(0)
#define W8(fid, val)  do { if (migbuf_u8_field(&b, (fid), (uint8_t)(val)) != DBRES_OK) goto fail; nfields++; } while(0)

    WS(MIGFIELD_TABLE,        desc->table);
    WS(MIGFIELD_NEW_NAME,     desc->new_name);
    WS(MIGFIELD_COL_NAME,     desc->col_name);

    if (desc->col_type != 0)
        W8(MIGFIELD_COL_TYPE, desc->col_type);

    W8(MIGFIELD_COL_NULLABLE, desc->col_nullable ? 1 : 0);
    W8(MIGFIELD_COL_HAS_DEF,  desc->col_has_default ? 1 : 0);

    if (desc->col_has_default && desc->col_default)
        WS(MIGFIELD_COL_DEFAULT, desc->col_default);

    // --- Column list for CREATE_TABLE ---
    if (desc->ncolumns > 0) {
        // compute compound field size
        migbuf_t cb;
        if (migbuf_init(&cb, 128) != DBRES_OK) goto fail;
        bool col_fail = false;

        if (migbuf_u16be(&cb, (uint16_t)desc->ncolumns) != DBRES_OK) col_fail = true;
        for (int i = 0; i < desc->ncolumns && !col_fail; i++) {
            const cloudsync_migration_column *c = &desc->columns[i];
            uint16_t nlen = c->name ? (uint16_t)strlen(c->name) : 0;
            if (migbuf_u16be(&cb, nlen)                                         != DBRES_OK) col_fail = true;
            if (!col_fail && nlen > 0 && migbuf_bytes(&cb, c->name, nlen)      != DBRES_OK) col_fail = true;
            if (!col_fail && migbuf_u8(&cb, (uint8_t)c->type)                  != DBRES_OK) col_fail = true;
            if (!col_fail && migbuf_u8(&cb, c->nullable ? 1 : 0)               != DBRES_OK) col_fail = true;
            uint8_t hd = (c->default_value != NULL) ? 1 : 0;
            if (!col_fail && migbuf_u8(&cb, hd)                                 != DBRES_OK) col_fail = true;
            if (!col_fail && hd) {
                uint32_t dlen = (uint32_t)strlen(c->default_value);
                if (migbuf_u32be(&cb, dlen)                                     != DBRES_OK) col_fail = true;
                if (!col_fail && migbuf_bytes(&cb, c->default_value, dlen)      != DBRES_OK) col_fail = true;
            }
            if (!col_fail && migbuf_u8(&cb, c->is_pk ? 1 : 0)                  != DBRES_OK) col_fail = true;
        }
        if (!col_fail) {
            if (migbuf_u8(&b, MIGFIELD_COLUMNS)        != DBRES_OK) { migbuf_free(&cb); goto fail; }
            if (migbuf_u32be(&b, (uint32_t)cb.len)     != DBRES_OK) { migbuf_free(&cb); goto fail; }
            if (migbuf_bytes(&b, cb.data, cb.len)      != DBRES_OK) { migbuf_free(&cb); goto fail; }
            nfields++;
        }
        migbuf_free(&cb);
        if (col_fail) goto fail;
    }

    // --- Index columns ---
    WS(MIGFIELD_INDEX_NAME, desc->index_name);
    if (desc->nindex_columns > 0) {
        migbuf_t ib;
        if (migbuf_init(&ib, 64) != DBRES_OK) goto fail;
        bool idx_fail = false;
        if (migbuf_u16be(&ib, (uint16_t)desc->nindex_columns) != DBRES_OK) idx_fail = true;
        for (int i = 0; i < desc->nindex_columns && !idx_fail; i++) {
            uint16_t nlen = desc->index_columns[i] ? (uint16_t)strlen(desc->index_columns[i]) : 0;
            if (migbuf_u16be(&ib, nlen)                                                        != DBRES_OK) idx_fail = true;
            if (!idx_fail && nlen > 0 && migbuf_bytes(&ib, desc->index_columns[i], nlen)      != DBRES_OK) idx_fail = true;
        }
        if (!idx_fail) {
            if (migbuf_u8(&b, MIGFIELD_INDEX_COLS)        != DBRES_OK) { migbuf_free(&ib); goto fail; }
            if (migbuf_u32be(&b, (uint32_t)ib.len)        != DBRES_OK) { migbuf_free(&ib); goto fail; }
            if (migbuf_bytes(&b, ib.data, ib.len)         != DBRES_OK) { migbuf_free(&ib); goto fail; }
            nfields++;
        }
        migbuf_free(&ib);
        if (idx_fail) goto fail;
    }
    if (desc->index_unique)
        W8(MIGFIELD_INDEX_UNIQUE, 1);

    if (desc->algo != 0)
        W8(MIGFIELD_ALGO, (uint8_t)desc->algo);

    WS(MIGFIELD_FILTER,      desc->filter);
    WS(MIGFIELD_SQL_SQLITE,  desc->sql_sqlite);
    WS(MIGFIELD_SQL_PGSQL,   desc->sql_postgresql);

#undef WS
#undef W8

    // Patch nfields back into the header
    b.data[nfields_pos]     = (uint8_t)((nfields >> 8) & 0xFF);
    b.data[nfields_pos + 1] = (uint8_t)(nfields & 0xFF);

    *out_blob = b.data;
    *out_size = b.len;
    return DBRES_OK;

fail:
    migbuf_free(&b);
    return rc;
}

cloudsync_migration_descriptor *cloudsync_migration_deserialize(const void *blob, size_t size) {
    if (!blob || size < 8) return NULL;  // 4 magic + 1 ver + 1 op + 2 nfields

    migrd_t r;
    r.data = (const uint8_t *)blob;
    r.len  = size;
    r.pos  = 0;

    // Validate magic
    uint32_t magic;
    if (migrd_u32be(&r, &magic)    != DBRES_OK) return NULL;
    if (magic != MIGR_MAGIC)                    return NULL;

    uint8_t  ver, op_byte;
    uint16_t nfields;
    if (migrd_u8(&r,    &ver)     != DBRES_OK) return NULL;
    if (ver != MIGR_VERSION)                   return NULL;
    if (migrd_u8(&r,    &op_byte) != DBRES_OK) return NULL;
    if (migrd_u16be(&r, &nfields) != DBRES_OK) return NULL;

    cloudsync_migration_descriptor *d = cloudsync_migration_create((cloudsync_migration_op)op_byte);
    if (!d) return NULL;

    for (uint16_t fi = 0; fi < nfields; fi++) {
        uint8_t  fid;
        uint32_t flen;
        if (migrd_u8(&r,    &fid)  != DBRES_OK) goto fail;
        if (migrd_u32be(&r, &flen) != DBRES_OK) goto fail;

        size_t field_end = r.pos + flen;
        if (field_end > r.len) goto fail;

        switch (fid) {
            case MIGFIELD_TABLE:    {
                if (migrd_string(&r, flen, &d->table)     != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_NEW_NAME: {
                if (migrd_string(&r, flen, &d->new_name)  != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_COL_NAME: {
                if (migrd_string(&r, flen, &d->col_name)  != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_COL_TYPE: {
                uint8_t v;
                if (migrd_u8(&r, &v) != DBRES_OK) goto fail;
                d->col_type = (cloudsync_column_type)v;
                break;
            }
            case MIGFIELD_COL_NULLABLE: {
                uint8_t v;
                if (migrd_u8(&r, &v) != DBRES_OK) goto fail;
                d->col_nullable = (v != 0);
                break;
            }
            case MIGFIELD_COL_HAS_DEF: {
                uint8_t v;
                if (migrd_u8(&r, &v) != DBRES_OK) goto fail;
                d->col_has_default = (v != 0);
                break;
            }
            case MIGFIELD_COL_DEFAULT: {
                if (migrd_string(&r, flen, &d->col_default) != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_COLUMNS: {
                migrd_t cr;
                cr.data = r.data + r.pos;
                cr.len  = flen;
                cr.pos  = 0;
                r.pos  += flen;  // advance outer reader past compound field

                uint16_t ncols;
                if (migrd_u16be(&cr, &ncols) != DBRES_OK) goto fail;
                for (uint16_t ci = 0; ci < ncols; ci++) {
                    uint16_t nlen;
                    if (migrd_u16be(&cr, &nlen) != DBRES_OK) goto fail;
                    char *cname = NULL;
                    if (nlen > 0) {
                        if (migrd_string(&cr, nlen, &cname) != DBRES_OK) goto fail;
                    } else {
                        cname = dup_str("");
                    }
                    uint8_t ctype, cnullable, chad;
                    if (migrd_u8(&cr, &ctype)     != DBRES_OK) { dbmem_free(cname); goto fail; }
                    if (migrd_u8(&cr, &cnullable)  != DBRES_OK) { dbmem_free(cname); goto fail; }
                    if (migrd_u8(&cr, &chad)       != DBRES_OK) { dbmem_free(cname); goto fail; }
                    char *cdef = NULL;
                    if (chad) {
                        uint32_t dlen;
                        if (migrd_u32be(&cr, &dlen)          != DBRES_OK) { dbmem_free(cname); goto fail; }
                        if (migrd_string(&cr, dlen, &cdef)   != DBRES_OK) { dbmem_free(cname); goto fail; }
                    }
                    uint8_t cpk;
                    if (migrd_u8(&cr, &cpk) != DBRES_OK) { dbmem_free(cname); dbmem_free(cdef); goto fail; }

                    // grow columns array
                    cloudsync_migration_column *cols =
                        (cloudsync_migration_column *)dbmem_realloc(d->columns,
                            (size_t)(d->ncolumns + 1) * sizeof(cloudsync_migration_column));
                    if (!cols) { dbmem_free(cname); dbmem_free(cdef); goto fail; }
                    d->columns = cols;
                    d->columns[d->ncolumns].name          = cname;
                    d->columns[d->ncolumns].type          = (cloudsync_column_type)ctype;
                    d->columns[d->ncolumns].nullable      = (cnullable != 0);
                    d->columns[d->ncolumns].default_value = cdef;
                    d->columns[d->ncolumns].is_pk         = (cpk != 0);
                    d->ncolumns++;
                }
                continue;  // r.pos already advanced above
            }
            case MIGFIELD_INDEX_NAME: {
                if (migrd_string(&r, flen, &d->index_name) != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_INDEX_COLS: {
                migrd_t ir;
                ir.data = r.data + r.pos;
                ir.len  = flen;
                ir.pos  = 0;
                r.pos  += flen;

                uint16_t nicols;
                if (migrd_u16be(&ir, &nicols) != DBRES_OK) goto fail;
                for (uint16_t ii = 0; ii < nicols; ii++) {
                    uint16_t nlen;
                    if (migrd_u16be(&ir, &nlen) != DBRES_OK) goto fail;
                    char *icname = NULL;
                    if (nlen > 0) {
                        if (migrd_string(&ir, nlen, &icname) != DBRES_OK) goto fail;
                    }
                    char **idxcols = (char **)dbmem_realloc(d->index_columns,
                        (size_t)(d->nindex_columns + 1) * sizeof(char *));
                    if (!idxcols) { dbmem_free(icname); goto fail; }
                    d->index_columns = idxcols;
                    d->index_columns[d->nindex_columns++] = icname;
                }
                continue;
            }
            case MIGFIELD_INDEX_UNIQUE: {
                uint8_t v;
                if (migrd_u8(&r, &v) != DBRES_OK) goto fail;
                d->index_unique = (v != 0);
                break;
            }
            case MIGFIELD_ALGO: {
                uint8_t v;
                if (migrd_u8(&r, &v) != DBRES_OK) goto fail;
                d->algo = (cloudsync_sync_algo)v;
                break;
            }
            case MIGFIELD_FILTER: {
                if (migrd_string(&r, flen, &d->filter) != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_SQL_SQLITE: {
                if (migrd_string(&r, flen, &d->sql_sqlite) != DBRES_OK) goto fail;
                break;
            }
            case MIGFIELD_SQL_PGSQL: {
                if (migrd_string(&r, flen, &d->sql_postgresql) != DBRES_OK) goto fail;
                break;
            }
            default:
                // Unknown field: skip it (forward-compatibility)
                r.pos += flen;
                continue;
        }
        // For non-compound fields, verify we consumed exactly flen bytes
        if (r.pos != field_end) {
            r.pos = field_end;  // re-sync to be safe
        }
    }

    return d;

fail:
    cloudsync_migration_free(d);
    return NULL;
}

// MARK: - Lazy migrations-table bootstrap -

// Ensures cloudsync_migrations exists, creating it if necessary.
// Called from the migration entry points rather than from dbutils_settings_init
// so that opening an existing database in a read-only or privilege-limited
// environment (which never uses the migration API) does not fail.
static int migration_ensure_table(cloudsync_context *ctx) {
    if (database_internal_table_exists(ctx, CLOUDSYNC_MIGRATIONS_NAME)) return DBRES_OK;
    return database_exec(ctx, SQL_CREATE_MIGRATIONS_TABLE);
}

// MARK: - Registration -

int cloudsync_migration_register(cloudsync_context *ctx, int64_t version,
                                  cloudsync_migration_descriptor *desc) {
    if (!ctx || !desc) return DBRES_MISUSE;

    // Ensure the base system tables exist (site_id, settings, …) then ensure
    // the migrations ledger exists.  Both steps are idempotent.
    if (!cloudsync_context_init(ctx)) return DBRES_MISUSE;
    if (migration_ensure_table(ctx) != DBRES_OK) return DBRES_ERROR;

    void   *blob = NULL;
    size_t  blob_size = 0;
    int rc = cloudsync_migration_serialize(desc, &blob, &blob_size);
    if (rc != DBRES_OK) return rc;

    uint64_t checksum = cloudsync_migration_checksum(blob, blob_size);

    dbvm_t *vm = NULL;
    rc = databasevm_prepare(ctx, SQL_MIGRATION_INSERT, &vm, 0);
    if (rc != DBRES_OK) goto done;

    // version
    if (databasevm_bind_int(vm, 1, version) != DBRES_OK) { rc = DBRES_ERROR; goto done; }
    // descriptor blob
    if (databasevm_bind_blob(vm, 2, blob, (uint64_t)blob_size) != DBRES_OK) { rc = DBRES_ERROR; goto done; }
    // checksum
    if (databasevm_bind_int(vm, 3, (int64_t)checksum) != DBRES_OK) { rc = DBRES_ERROR; goto done; }

    rc = databasevm_step(vm);
    if (rc == DBRES_DONE) rc = DBRES_OK;

done:
    databasevm_finalize(vm);
    dbmem_free(blob);
    return rc;
}

// MARK: - Apply pending -

int cloudsync_migration_apply_pending(cloudsync_context *ctx) {
    if (!ctx) return DBRES_MISUSE;

    // Ensure the base system tables then the migrations ledger both exist.
    if (!cloudsync_context_init(ctx)) return DBRES_MISUSE;
    if (migration_ensure_table(ctx) != DBRES_OK) return DBRES_ERROR;

    dbvm_t *vm = NULL;
    int rc = databasevm_prepare(ctx, SQL_MIGRATION_SELECT_PENDING, &vm, 0);
    if (rc != DBRES_OK) return rc;

    // Collect versions + blobs first (can't execute DDL while iterating)
    typedef struct { int64_t version; void *blob; size_t blen; uint64_t checksum; } pending_t;
    pending_t *pending  = NULL;
    int        npending = 0;

    while ((rc = databasevm_step(vm)) == DBRES_ROW) {
        int64_t  ver      = database_column_int(vm, 0);
        int      blen     = database_column_bytes(vm, 1);
        const void *bptr  = database_column_blob(vm, 1, NULL);
        int64_t  csum     = database_column_int(vm, 2);

        void *blob_copy = dbmem_alloc((size_t)blen);
        if (!blob_copy) { rc = DBRES_NOMEM; break; }
        memcpy(blob_copy, bptr, (size_t)blen);

        pending_t *p = (pending_t *)dbmem_realloc(pending, (size_t)(npending + 1) * sizeof(pending_t));
        if (!p) { dbmem_free(blob_copy); rc = DBRES_NOMEM; break; }
        pending = p;
        pending[npending].version  = ver;
        pending[npending].blob     = blob_copy;
        pending[npending].blen     = (size_t)blen;
        pending[npending].checksum = (uint64_t)csum;
        npending++;
    }
    databasevm_finalize(vm);
    vm = NULL;

    if (rc == DBRES_DONE) rc = DBRES_OK;
    if (rc != DBRES_OK) goto cleanup;

    if (npending == 0) goto cleanup;

    // Snapshot the table count before the batch.  Used with batch_enrolled_any
    // below to decide whether the last-table epilogue should run: cleanup is
    // needed when the batch ends at zero AND either (a) tables were tracked
    // before the batch, or (b) an INIT_SYNC within the batch enrolled at least
    // one table that was then dropped.  Purely local DDL batches (never enrolled
    // any table) must not trigger cleanup even if they also end at zero.
    int pre_batch_tables = cloudsync_tables_count(ctx);
    bool batch_enrolled_any = false;  // set to true when INIT_SYNC succeeds

    // Declare before the savepoint so no jump skips the initialisation.
    dbvm_t *mark_vm = NULL;

    // Wrap the entire batch in a savepoint for atomicity: if any migration
    // fails, all DDL changes are rolled back so no half-applied schema is
    // committed and a retry starts from a clean state.
    rc = database_begin_savepoint(ctx, "cloudsync_migration_batch");
    if (rc != DBRES_OK) goto cleanup;

    rc = databasevm_prepare(ctx, SQL_MIGRATION_MARK_APPLIED, &mark_vm, 0);
    if (rc != DBRES_OK) {
        database_rollback_savepoint(ctx, "cloudsync_migration_batch");
        goto cleanup;
    }

    for (int i = 0; i < npending; i++) {
        // Verify checksum
        uint64_t expected = cloudsync_migration_checksum(pending[i].blob, pending[i].blen);
        if (expected != pending[i].checksum) {
            rc = DBRES_ERROR;
            break;
        }

        cloudsync_migration_descriptor *desc =
            cloudsync_migration_deserialize(pending[i].blob, pending[i].blen);
        if (!desc) { rc = DBRES_ERROR; break; }

        bool needs_alter = migration_needs_alter_lifecycle(desc->op);

        // Resolve the bare table name for the alter lifecycle.  The in-memory
        // registry stores bare names, so "sales.orders" must be matched against
        // "orders".  When a schema prefix is present, also switch the context
        // schema so that cloudsync_begin_alter / cloudsync_commit_alter (which
        // resolve schema via cloudsync_schema) target the right schema.
        char alter_schema_buf[256] = {0};
        const char *alter_tname = desc->table;
        if (needs_alter && desc->table) {
            const char *bare = migration_split_name(desc->table,
                                                     alter_schema_buf,
                                                     sizeof(alter_schema_buf));
            if (alter_schema_buf[0]) alter_tname = bare;
        }

        // Only enter the alter lifecycle for tables already enrolled in CloudSync.
        // A table created via CREATE_TABLE but not yet INIT_SYNC'd is local-only;
        // its DDL is valid but does not need trigger/shadow management.
        //
        // When the descriptor carries an explicit schema qualifier (e.g. "sales.orders"),
        // require the registered entry's schema to match.  Without this check,
        // table_lookup("orders") would return a "public.orders" entry and incorrectly
        // enter the alter lifecycle for an unrelated table in a different schema.
        bool is_tracked = false;
        if (needs_alter && desc->table) {
            cloudsync_table_context *found = table_lookup(ctx, alter_tname);
            if (found) {
                if (alter_schema_buf[0] == '\0') {
                    // No schema qualifier in the descriptor — name-only match is correct.
                    is_tracked = true;
                } else {
                    // Schema qualifier present: the registered entry's schema must match.
                    // A NULL or empty entry_schema means the table was enrolled without
                    // an explicit schema context (common in SQLite where "main" is
                    // implicit, or in PostgreSQL before schema support was added).
                    // Treat that as a match so that a descriptor using "main.orders"
                    // or "public.orders" still finds a table enrolled as plain "orders".
                    const char *entry_schema = table_get_schema(found);
                    is_tracked = (entry_schema == NULL || entry_schema[0] == '\0' ||
                                  strcasecmp(entry_schema, alter_schema_buf) == 0);
                }
            }
        }

        char alter_saved_schema_buf[256] = {0};
        const char *alter_saved_schema = NULL;
        bool alter_schema_switched = false;
        if (is_tracked && alter_schema_buf[0]) {
            const char *_s = cloudsync_schema(ctx);
            if (_s) snprintf(alter_saved_schema_buf, sizeof(alter_saved_schema_buf), "%s", _s);
            alter_saved_schema = alter_saved_schema_buf[0] ? alter_saved_schema_buf : NULL;
            cloudsync_set_schema(ctx, alter_schema_buf);
            alter_schema_switched = true;
        }

        if (is_tracked) {
            rc = cloudsync_begin_alter(ctx, alter_tname);
            if (rc != DBRES_OK) {
                if (alter_schema_switched) cloudsync_set_schema(ctx, alter_saved_schema);
                cloudsync_migration_free(desc);
                break;
            }
        }

        if (desc->op == CLOUDSYNC_MIGRATION_INIT_SYNC) {
            // Split any schema prefix from desc->table so that metadata helpers
            // receive a bare name.  dbutils_table_settings_set_key_value and
            // cloudsync_init_table both key on the bare name plus the context
            // schema; passing "sales.orders" directly would cause init to look
            // for a table literally named "sales.orders" in the current schema.
            char init_schema_buf[256] = {0};
            const char *init_tname = desc->table
                ? migration_split_name(desc->table, init_schema_buf, sizeof(init_schema_buf))
                : NULL;
            char init_saved_schema_buf[256] = {0};
            const char *init_saved_schema = NULL;
            bool init_schema_switched = false;
            if (init_schema_buf[0]) {
                const char *_s = cloudsync_schema(ctx);
                if (_s) snprintf(init_saved_schema_buf, sizeof(init_saved_schema_buf), "%s", _s);
                init_saved_schema = init_saved_schema_buf[0] ? init_saved_schema_buf : NULL;
                cloudsync_set_schema(ctx, init_schema_buf);
                init_schema_switched = true;
            }
            // Store the row filter BEFORE cloudsync_init_table so that the
            // trigger-creation path inside init picks it up automatically.
            if (desc->filter && init_tname) {
                rc = dbutils_table_settings_set_key_value(ctx, init_tname,
                                                          "*", "filter",
                                                          desc->filter);
                if (rc != DBRES_OK) {
                    if (init_schema_switched) cloudsync_set_schema(ctx, init_saved_schema);
                    cloudsync_migration_free(desc);
                    break;
                }
            }
            const char *algo_str = cloudsync_sync_algo_name(desc->algo);
            if (!algo_str) algo_str = CLOUDSYNC_DEFAULT_ALGO;
            rc = cloudsync_init_table(ctx, init_tname, algo_str, CLOUDSYNC_INIT_FLAG_NONE);
            if (init_schema_switched) cloudsync_set_schema(ctx, init_saved_schema);
            if (rc == DBRES_OK) batch_enrolled_any = true;
        } else if (desc->op == CLOUDSYNC_MIGRATION_DROP_TABLE && desc->table) {
            rc = migration_apply_drop_table(ctx, desc);
        } else if (desc->op == CLOUDSYNC_MIGRATION_RENAME_TABLE &&
                   desc->table && desc->new_name) {
            rc = migration_apply_rename_table(ctx, desc);
        } else {
            rc = database_migration_execute(ctx, desc);
        }

        if (rc != DBRES_OK) {
            if (is_tracked) {
                cloudsync_commit_alter(ctx, alter_tname);  // best-effort rollback
                if (alter_schema_switched) cloudsync_set_schema(ctx, alter_saved_schema);
            }
            cloudsync_migration_free(desc);
            break;
        }

        if (is_tracked) {
            rc = cloudsync_commit_alter(ctx, alter_tname);
            if (alter_schema_switched) cloudsync_set_schema(ctx, alter_saved_schema);
            if (rc != DBRES_OK) { cloudsync_migration_free(desc); break; }
        }

        cloudsync_migration_free(desc);

        // Mark as applied
        int64_t now = (int64_t)time(NULL);
        databasevm_reset(mark_vm);
        databasevm_bind_int(mark_vm, 1, now);
        databasevm_bind_int(mark_vm, 2, pending[i].version);
        int step_rc = databasevm_step(mark_vm);
        if (step_rc != DBRES_DONE && step_rc != DBRES_OK) {
            rc = DBRES_ERROR;
            break;
        }
    }

    databasevm_finalize(mark_vm);

    if (rc == DBRES_OK) {
        // Capture the release return value: for some backends (e.g. PostgreSQL
        // with deferred constraint checks) the error surfaces only at RELEASE
        // SAVEPOINT.  Ignoring this would report success while the DDL and
        // applied_at updates have actually been rolled back by the failed release.
        rc = database_commit_savepoint(ctx, "cloudsync_migration_batch");
        if (rc == DBRES_OK) {
            if (cloudsync_tables_count(ctx) == 0 &&
                (pre_batch_tables > 0 || batch_enrolled_any)) {
                // The batch ended with zero tracked tables AND at some point
                // during the batch at least one table was (or had been) enrolled
                // in CloudSync.  Run the global last-table epilogue.
                //
                // The two conditions together handle both cases:
                //   • pre_batch_tables > 0: tables existed before the batch
                //     and were all dropped by it (the common path).
                //   • batch_enrolled_any: a cold-start history replay enrolled
                //     tables via INIT_SYNC and then dropped them all within the
                //     same batch (0 → N → 0).  Without this flag the stale
                //     global metadata created by INIT_SYNC would be left behind.
                //
                // Purely local DDL batches (no INIT_SYNC, no pre-existing tracked
                // tables) are intentionally excluded: they never write global
                // metadata, so there is nothing to clean up.
                //   cloudsync_reset_siteid     — clears the in-memory replica
                //                               identity so a subsequent
                //                               cloudsync_init() starts fresh
                //   dbutils_settings_cleanup   — drops cloudsync_settings,
                //                               cloudsync_site_id,
                //                               cloudsync_table_settings,
                //                               cloudsync_schema_versions
                // Schema-hash refresh is intentionally skipped: the schema-
                // versions table is about to be dropped, and advertising a hash
                // over an empty schema is meaningless to peers.
                cloudsync_reset_siteid(ctx);
                rc = dbutils_settings_cleanup(ctx);
            } else {
                // Refresh the schema hash so subsequent syncs advertise the
                // post-migration schema; without this the old hash is retained
                // until the extension is reinitialized, causing peers to reject
                // payloads.
                cloudsync_update_schema_hash(ctx);
            }
        } else {
            // The savepoint release failed — the DB rolled back the entire batch.
            // Resync the in-memory context so it matches the reverted DB state.
            cloudsync_reload_tables(ctx);
        }
    } else {
        database_rollback_savepoint(ctx, "cloudsync_migration_batch");
        // Re-sync the in-memory context with the rolled-back DB state.  Earlier
        // migrations in this batch may have mutated ctx (INIT_SYNC added tables,
        // DROP_TABLE called cloudsync_forget_table, RENAME_TABLE rewrote entries,
        // commit_alter reloaded schemas); the savepoint reverted the DB but not
        // the in-memory list.  cloudsync_reload_tables evicts stale entries and
        // repopulates from the clean DB.
        cloudsync_reload_tables(ctx);
    }

cleanup:
    for (int i = 0; i < npending; i++) dbmem_free(pending[i].blob);
    dbmem_free(pending);
    return rc;
}
