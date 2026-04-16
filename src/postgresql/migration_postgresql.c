//
//  migration_postgresql.c
//  cloudsync
//
//  PostgreSQL-specific DDL generation and execution for the migration system.
//  Uses standard snprintf and manual identifier quoting (no sqlite3_mprintf).
//
//  Created by Marco Bambini on 15/04/26.
//

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "migration.h"
#include "database.h"
#include "../cloudsync.h"

// MARK: - Type mapping -

// Maps CSTYPE_* to the PostgreSQL DDL type name for CREATE TABLE / ADD COLUMN.
static const char *cstype_to_postgresql(cloudsync_column_type t) {
    switch (t) {
        case CSTYPE_INTEGER:   return "INTEGER";
        case CSTYPE_REAL:      return "DOUBLE PRECISION";
        case CSTYPE_TEXT:      return "TEXT";
        case CSTYPE_BLOB:      return "BYTEA";
        case CSTYPE_BOOLEAN:   return "BOOLEAN";
        case CSTYPE_UUID:      return "UUID";
        case CSTYPE_TIMESTAMP: return "BIGINT";
        case CSTYPE_JSON:      return "JSONB";
        default:               return "TEXT";
    }
}

// MARK: - Identifier quoting -

// Returns a newly-allocated double-quoted SQL identifier.
// Internal double-quotes are escaped by doubling (SQL standard).
static char *quote_ident(const char *name) {
    if (!name) return NULL;

    // Worst case: every char is '"', so we need 2x len + 2 quotes + NUL
    size_t nlen = strlen(name);
    char *buf = (char *)dbmem_alloc(nlen * 2 + 3);
    if (!buf) return NULL;

    size_t pos = 0;
    buf[pos++] = '"';
    for (size_t i = 0; i < nlen; i++) {
        if (name[i] == '"') buf[pos++] = '"';
        buf[pos++] = name[i];
    }
    buf[pos++] = '"';
    buf[pos]   = '\0';
    return buf;
}

// MARK: - Dynamic string builder -
// Simple append-only buffer using dbmem_realloc.

typedef struct { char *data; size_t len; size_t cap; } pgstr_t;

static int pgstr_init(pgstr_t *s, size_t initial) {
    s->data = (char *)dbmem_alloc(initial);
    if (!s->data) return DBRES_NOMEM;
    s->data[0] = '\0';
    s->len = 0;
    s->cap = initial;
    return DBRES_OK;
}

static void pgstr_free(pgstr_t *s) {
    if (s->data) dbmem_free(s->data);
    s->data = NULL; s->len = s->cap = 0;
}

static int pgstr_append(pgstr_t *s, const char *text) {
    if (!text) return DBRES_OK;
    size_t tlen = strlen(text);
    if (s->len + tlen + 1 > s->cap) {
        size_t nc = (s->cap * 2 > s->len + tlen + 1) ? s->cap * 2 : s->len + tlen + 64;
        char *p = (char *)dbmem_realloc(s->data, nc);
        if (!p) return DBRES_NOMEM;
        s->data = p; s->cap = nc;
    }
    memcpy(s->data + s->len, text, tlen);
    s->len += tlen;
    s->data[s->len] = '\0';
    return DBRES_OK;
}

// Append a double-quoted identifier
static int pgstr_append_ident(pgstr_t *s, const char *name) {
    char *q = quote_ident(name);
    if (!q) return DBRES_NOMEM;
    int rc = pgstr_append(s, q);
    dbmem_free(q);
    return rc;
}

// Append a schema-qualified identifier reference.
// Three cases:
//   1. 'name' already contains '.'  → split on first dot and quote each part:
//      "sales.orders" → "sales"."orders"
//   2. 'fallback_schema' is non-NULL and 'name' is unqualified:
//      prefix with the schema: "public"."orders"
//   3. Otherwise emit just the double-quoted bare name: "orders"
//
// Use this for table and index names in DDL; NOT for column names or for
// RENAME TABLE / RENAME COLUMN destination names (which must be bare).
static int pgstr_append_table_ref(pgstr_t *s, const char *name, const char *fallback_schema) {
    if (!name) return DBRES_ERROR;
    const char *dot = strchr(name, '.');
    if (dot) {
        // Explicit schema.name: quote each part separately
        size_t slen = (size_t)(dot - name);
        if (slen == 0 || slen >= 256) return DBRES_ERROR;
        char schema_part[256];
        memcpy(schema_part, name, slen);
        schema_part[slen] = '\0';
        if (pgstr_append_ident(s, schema_part) != DBRES_OK) return DBRES_NOMEM;
        if (pgstr_append(s, ".")               != DBRES_OK) return DBRES_NOMEM;
        return pgstr_append_ident(s, dot + 1);
    }
    if (fallback_schema) {
        if (pgstr_append_ident(s, fallback_schema) != DBRES_OK) return DBRES_NOMEM;
        if (pgstr_append(s, ".")                   != DBRES_OK) return DBRES_NOMEM;
    }
    return pgstr_append_ident(s, name);
}

// Append a formatted string (snprintf helper)
static int pgstr_appendf(pgstr_t *s, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return DBRES_ERROR;
    return pgstr_append(s, tmp);
}

// MARK: - Column definition builder -

// Returns a heap-allocated column definition: "name" TYPE [NOT NULL] [DEFAULT ...]
static char *build_col_def(const char *name, cloudsync_column_type type,
                            bool nullable, bool has_default, const char *default_val) {
    pgstr_t s;
    if (pgstr_init(&s, 128) != DBRES_OK) return NULL;

    if (pgstr_append_ident(&s, name)             != DBRES_OK) goto fail;
    if (pgstr_appendf(&s, " %s", cstype_to_postgresql(type)) != DBRES_OK) goto fail;

    if (!nullable) {
        if (pgstr_append(&s, " NOT NULL") != DBRES_OK) goto fail;
    }

    if (has_default && default_val) {
        if (pgstr_appendf(&s, " DEFAULT %s", default_val) != DBRES_OK) goto fail;
    }

    return s.data;   // caller owns this

fail:
    pgstr_free(&s);
    return NULL;
}

// MARK: - SQL generation -

// Internal generator that accepts an optional schema for qualification.
// 'schema' may be NULL (unqualified output — used by the public API and tests).
// database_migration_execute passes cloudsync_schema(ctx) so DDL targets the
// correct schema when synced tables live outside the default search_path.
//
// Table / index name quoting rules:
//   • desc->table and desc->index_name use pgstr_append_table_ref so that an
//     already schema-qualified name like "sales.orders" produces "sales"."orders"
//     and an unqualified name gets the fallback schema prefix when one is set.
//   • RENAME TABLE destination (desc->new_name) uses bare pgstr_append_ident
//     because PostgreSQL's RENAME TO clause must not carry a schema qualifier.
//   • Column names always use pgstr_append_ident (no schema).
static char *build_migration_sql(const cloudsync_migration_descriptor *desc,
                                  const char *schema) {
    if (!desc) return NULL;

    pgstr_t s;
    if (pgstr_init(&s, 256) != DBRES_OK) return NULL;

    switch (desc->op) {

        case CLOUDSYNC_MIGRATION_ADD_COLUMN: {
            if (!desc->table || !desc->col_name) goto fail;
            char *col = build_col_def(desc->col_name, desc->col_type,
                                       desc->col_nullable, desc->col_has_default,
                                       desc->col_default);
            if (!col) goto fail;
            if (pgstr_append(&s, "ALTER TABLE ")              != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema) != DBRES_OK ||
                pgstr_append(&s, " ADD COLUMN ")              != DBRES_OK ||
                pgstr_append(&s, col)                         != DBRES_OK ||
                pgstr_append(&s, ";")                         != DBRES_OK) {
                dbmem_free(col); goto fail;
            }
            dbmem_free(col);
            break;
        }

        case CLOUDSYNC_MIGRATION_DROP_COLUMN: {
            if (!desc->table || !desc->col_name) goto fail;
            if (pgstr_append(&s, "ALTER TABLE ")               != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema) != DBRES_OK ||
                pgstr_append(&s, " DROP COLUMN ")              != DBRES_OK ||
                pgstr_append_ident(&s, desc->col_name)         != DBRES_OK ||
                pgstr_append(&s, ";")                          != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_RENAME_COLUMN: {
            if (!desc->table || !desc->col_name || !desc->new_name) goto fail;
            if (pgstr_append(&s, "ALTER TABLE ")               != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema) != DBRES_OK ||
                pgstr_append(&s, " RENAME COLUMN ")            != DBRES_OK ||
                pgstr_append_ident(&s, desc->col_name)         != DBRES_OK ||
                pgstr_append(&s, " TO ")                       != DBRES_OK ||
                pgstr_append_ident(&s, desc->new_name)         != DBRES_OK ||
                pgstr_append(&s, ";")                          != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_SET_DEFAULT: {
            if (!desc->table || !desc->col_name || !desc->col_default) goto fail;
            if (pgstr_append(&s, "ALTER TABLE ")               != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema) != DBRES_OK ||
                pgstr_append(&s, " ALTER COLUMN ")             != DBRES_OK ||
                pgstr_append_ident(&s, desc->col_name)         != DBRES_OK ||
                pgstr_appendf(&s, " SET DEFAULT %s;", desc->col_default) != DBRES_OK)
                goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_CREATE_TABLE: {
            if (!desc->table || desc->ncolumns == 0) goto fail;

            // Count PK columns: with more than one the constraint must be
            // table-level — inline PRIMARY KEY on multiple columns is invalid.
            int npks = 0;
            for (int i = 0; i < desc->ncolumns; i++) {
                if (desc->columns[i].is_pk) npks++;
            }

            if (pgstr_append(&s, "CREATE TABLE IF NOT EXISTS ")  != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema)  != DBRES_OK ||
                pgstr_append(&s, " (")                           != DBRES_OK) goto fail;

            for (int i = 0; i < desc->ncolumns; i++) {
                const cloudsync_migration_column *c = &desc->columns[i];
                char *col = build_col_def(c->name, c->type, c->nullable,
                                           c->default_value != NULL, c->default_value);
                if (!col) goto fail;
                const char *sep = (i > 0) ? ", " : "";
                // Use inline PRIMARY KEY only for sole PK; composite uses table constraint.
                const char *pk  = (c->is_pk && npks == 1) ? " PRIMARY KEY" : "";
                if (pgstr_appendf(&s, "%s", sep)  != DBRES_OK ||
                    pgstr_append(&s, col)          != DBRES_OK ||
                    pgstr_append(&s, pk)           != DBRES_OK) {
                    dbmem_free(col); goto fail;
                }
                dbmem_free(col);
            }

            // Emit the table-level PRIMARY KEY constraint for composite PKs.
            if (npks > 1) {
                if (pgstr_append(&s, ", PRIMARY KEY (") != DBRES_OK) goto fail;
                bool first_pk = true;
                for (int i = 0; i < desc->ncolumns; i++) {
                    if (!desc->columns[i].is_pk) continue;
                    const char *sep = first_pk ? "" : ", ";
                    first_pk = false;
                    if (pgstr_append(&s, sep)                          != DBRES_OK ||
                        pgstr_append_ident(&s, desc->columns[i].name)  != DBRES_OK) goto fail;
                }
                if (pgstr_append(&s, ")") != DBRES_OK) goto fail;
            }

            if (pgstr_append(&s, ");") != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_DROP_TABLE: {
            if (!desc->table) goto fail;
            if (pgstr_append(&s, "DROP TABLE IF EXISTS ")        != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema)  != DBRES_OK ||
                pgstr_append(&s, ";")                            != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_RENAME_TABLE: {
            if (!desc->table || !desc->new_name) goto fail;
            // RENAME TO does NOT accept a schema qualifier on the new name:
            // PostgreSQL keeps the table in its current schema and treats any
            // dot in the identifier as part of a literal name, not as a schema
            // separator.  Strip any schema prefix from desc->new_name so that
            // RENAME TO always receives a bare identifier.
            const char *new_bare = desc->new_name;
            const char *new_dot  = strchr(desc->new_name, '.');
            if (new_dot) new_bare = new_dot + 1;
            if (pgstr_append(&s, "ALTER TABLE ")               != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema) != DBRES_OK ||
                pgstr_append(&s, " RENAME TO ")                != DBRES_OK ||
                pgstr_append_ident(&s, new_bare)               != DBRES_OK ||
                pgstr_append(&s, ";")                          != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_CREATE_INDEX: {
            if (!desc->index_name || !desc->table || desc->nindex_columns == 0) goto fail;
            const char *unique_kw = desc->index_unique ? "UNIQUE " : "";
            // PostgreSQL requires an index to live in the same schema as its
            // table.  Derive the effective index schema from desc->table (split
            // on '.') so that unqualified index names are placed in the correct
            // schema rather than the context schema when the table lives in a
            // non-default schema (e.g. sales.orders → index in sales, not public).
            const char *idx_schema = schema;
            char tbl_schema_buf[256] = {0};
            const char *tbl_dot = strchr(desc->table, '.');
            if (tbl_dot) {
                size_t slen = (size_t)(tbl_dot - desc->table);
                if (slen > 0 && slen < sizeof(tbl_schema_buf)) {
                    memcpy(tbl_schema_buf, desc->table, slen);
                    tbl_schema_buf[slen] = '\0';
                    idx_schema = tbl_schema_buf;
                }
            }
            if (pgstr_appendf(&s, "CREATE %sINDEX IF NOT EXISTS ", unique_kw)    != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->index_name, idx_schema)         != DBRES_OK ||
                pgstr_append(&s, " ON ")                                          != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->table, schema)                  != DBRES_OK ||
                pgstr_append(&s, " (")                                            != DBRES_OK) goto fail;

            for (int i = 0; i < desc->nindex_columns; i++) {
                const char *sep = (i > 0) ? ", " : "";
                if (pgstr_append(&s, sep)                              != DBRES_OK ||
                    pgstr_append_ident(&s, desc->index_columns[i])     != DBRES_OK) goto fail;
            }
            if (pgstr_append(&s, ");") != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_DROP_INDEX: {
            if (!desc->index_name) goto fail;
            // Derive the index schema in priority order:
            //  1. Schema embedded in desc->index_name itself ("sales.idx_orders").
            //     pgstr_append_table_ref splits on the dot and ignores the fallback,
            //     so setting drop_idx_schema here is redundant for that path but
            //     makes the intent explicit.
            //  2. Schema from desc->table ("sales.orders" → "sales").  DROP_INDEX
            //     descriptors do not require desc->table, so this is a secondary hint.
            //  3. Current context schema (last resort).
            const char *drop_idx_schema = schema;
            char drop_idx_schema_buf[256] = {0};
            const char *idx_dot = strchr(desc->index_name, '.');
            if (idx_dot) {
                // Priority 1: qualified index name — extract its schema prefix.
                size_t slen = (size_t)(idx_dot - desc->index_name);
                if (slen > 0 && slen < sizeof(drop_idx_schema_buf)) {
                    memcpy(drop_idx_schema_buf, desc->index_name, slen);
                    drop_idx_schema_buf[slen] = '\0';
                    drop_idx_schema = drop_idx_schema_buf;
                }
            } else if (desc->table) {
                // Priority 2: bare index name — try to pull schema from desc->table.
                const char *tbl_dot = strchr(desc->table, '.');
                if (tbl_dot) {
                    size_t slen = (size_t)(tbl_dot - desc->table);
                    if (slen > 0 && slen < sizeof(drop_idx_schema_buf)) {
                        memcpy(drop_idx_schema_buf, desc->table, slen);
                        drop_idx_schema_buf[slen] = '\0';
                        drop_idx_schema = drop_idx_schema_buf;
                    }
                }
            }
            if (pgstr_append(&s, "DROP INDEX IF EXISTS ")                    != DBRES_OK ||
                pgstr_append_table_ref(&s, desc->index_name, drop_idx_schema) != DBRES_OK ||
                pgstr_append(&s, ";")                                        != DBRES_OK) goto fail;
            break;
        }

        case CLOUDSYNC_MIGRATION_INIT_SYNC:
            // Handled in migration.c via cloudsync_init_table(); no DDL here.
            pgstr_free(&s);
            return NULL;

        case CLOUDSYNC_MIGRATION_CUSTOM:
            if (!desc->sql_postgresql) goto fail;
            pgstr_free(&s);
            {
                size_t n = strlen(desc->sql_postgresql);
                char *copy = (char *)dbmem_alloc(n + 1);
                if (!copy) return NULL;
                memcpy(copy, desc->sql_postgresql, n + 1);
                return copy;
            }

        default:
            goto fail;
    }

    return s.data;  // caller owns this

fail:
    pgstr_free(&s);
    return NULL;
}

// Public API: no schema context available — produces unqualified identifiers.
// Used by tests and any caller that only needs the SQL text.
char *database_migration_sql(const cloudsync_migration_descriptor *desc) {
    return build_migration_sql(desc, NULL);
}

// MARK: - Execution -

int database_migration_execute(cloudsync_context *ctx,
                                cloudsync_migration_descriptor *desc) {
    if (!ctx || !desc) return DBRES_MISUSE;

    // INIT_SYNC is handled by the caller (migration.c).
    if (desc->op == CLOUDSYNC_MIGRATION_INIT_SYNC) return DBRES_OK;

    // Pass the context schema so table names in DDL are correctly qualified
    // when the synced tables live outside the current search_path schema.
    char *sql = build_migration_sql(desc, cloudsync_schema(ctx));
    if (!sql) return DBRES_ERROR;

    int rc = database_exec(ctx, sql);
    dbmem_free(sql);
    return rc;
}
