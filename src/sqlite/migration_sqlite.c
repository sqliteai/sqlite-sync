//
//  migration_sqlite.c
//  cloudsync
//
//  SQLite-specific DDL generation and execution for the migration system.
//  Uses sqlite3_mprintf format specifiers: %w to escape embedded double-quotes
//  within an identifier (it doubles any '"' in the argument but does NOT add
//  surrounding '"' characters itself).  All identifier uses therefore wrap the
//  specifier in explicit \"...\" so the output is always a valid double-quoted
//  SQLite identifier regardless of whether the name is a keyword, contains
//  spaces, or relies on case-sensitive quoting.
//  %q is used for single-quoted string literals.
//
//  Created by Marco Bambini on 15/04/26.
//

#include <string.h>
#include "migration.h"
#include "database.h"

// MARK: - Type mapping -

// Maps CSTYPE_* to the SQLite DDL keyword for CREATE TABLE / ADD COLUMN.
static const char *cstype_to_sqlite(cloudsync_column_type t) {
    switch (t) {
        case CSTYPE_INTEGER:   return "INTEGER";
        case CSTYPE_REAL:      return "REAL";
        case CSTYPE_TEXT:      return "TEXT";
        case CSTYPE_BLOB:      return "BLOB";
        case CSTYPE_BOOLEAN:   return "INTEGER";   // no native BOOLEAN in SQLite
        case CSTYPE_UUID:      return "TEXT";       // UUIDs stored as TEXT
        case CSTYPE_TIMESTAMP: return "INTEGER";   // Unix timestamp as INTEGER
        case CSTYPE_JSON:      return "TEXT";       // JSON stored as TEXT
        default:               return "TEXT";
    }
}

// MARK: - Column definition builder -

// Returns a heap-allocated column definition fragment suitable for use in
// CREATE TABLE or ADD COLUMN:  "name" TYPE [NOT NULL] [DEFAULT ...]
static char *build_col_def(const char *name, cloudsync_column_type type,
                            bool nullable, bool has_default, const char *default_val) {
    const char *type_str = cstype_to_sqlite(type);

    // \"%w\" — explicit surrounding double-quotes; %w escapes embedded '"'.
    char *base = dbmem_mprintf("\"%w\" %s", name, type_str);
    if (!base) return NULL;

    if (!nullable) {
        char *tmp = dbmem_mprintf("%s NOT NULL", base);
        dbmem_free(base);
        if (!tmp) return NULL;
        base = tmp;
    }

    if (has_default && default_val) {
        char *tmp = dbmem_mprintf("%s DEFAULT %s", base, default_val);
        dbmem_free(base);
        if (!tmp) return NULL;
        base = tmp;
    }

    return base;
}

// MARK: - SQL generation -

char *database_migration_sql(const cloudsync_migration_descriptor *desc) {
    if (!desc) return NULL;

    // Strip any "schema." qualifier from table/new_name before building SQL.
    // Migration descriptors may originate from a PostgreSQL deployment that uses
    // qualified names like "sales.orders".  SQLite has no "sales" schema — the
    // table lives in "main" under its bare name.  Using the qualified string as a
    // single identifier would produce ALTER TABLE "sales.orders" which SQLite
    // interprets as a literal table name, not a schema-qualified reference.
    const char *bare_table = desc->table;
    if (bare_table) {
        const char *dot = strchr(bare_table, '.');
        if (dot) bare_table = dot + 1;
    }
    const char *bare_new_name = desc->new_name;
    if (bare_new_name) {
        const char *dot = strchr(bare_new_name, '.');
        if (dot) bare_new_name = dot + 1;
    }

    switch (desc->op) {

        case CLOUDSYNC_MIGRATION_ADD_COLUMN: {
            if (!bare_table || !desc->col_name) return NULL;
            char *col = build_col_def(desc->col_name, desc->col_type,
                                       desc->col_nullable, desc->col_has_default,
                                       desc->col_default);
            if (!col) return NULL;
            char *sql = dbmem_mprintf("ALTER TABLE \"%w\" ADD COLUMN %s;",
                                      bare_table, col);
            dbmem_free(col);
            return sql;
        }

        case CLOUDSYNC_MIGRATION_DROP_COLUMN: {
            if (!bare_table || !desc->col_name) return NULL;
            return dbmem_mprintf("ALTER TABLE \"%w\" DROP COLUMN \"%w\";",
                                 bare_table, desc->col_name);
        }

        case CLOUDSYNC_MIGRATION_RENAME_COLUMN: {
            if (!bare_table || !desc->col_name || !bare_new_name) return NULL;
            return dbmem_mprintf("ALTER TABLE \"%w\" RENAME COLUMN \"%w\" TO \"%w\";",
                                 bare_table, desc->col_name, bare_new_name);
        }

        case CLOUDSYNC_MIGRATION_SET_DEFAULT: {
            // SQLite ALTER TABLE does not support SET DEFAULT.
            // Use CLOUDSYNC_MIGRATION_CUSTOM with explicit SQL for this case.
            return NULL;
        }

        case CLOUDSYNC_MIGRATION_CREATE_TABLE: {
            if (!bare_table || desc->ncolumns == 0) return NULL;

            // Count PK columns: with more than one the constraint must be
            // table-level — inline PRIMARY KEY on multiple columns is invalid.
            int npks = 0;
            for (int i = 0; i < desc->ncolumns; i++) {
                if (desc->columns[i].is_pk) npks++;
            }

            char *sql = dbmem_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (", bare_table);
            if (!sql) return NULL;

            for (int i = 0; i < desc->ncolumns; i++) {
                const cloudsync_migration_column *c = &desc->columns[i];
                char *col = build_col_def(c->name, c->type, c->nullable,
                                           c->default_value != NULL, c->default_value);
                if (!col) { dbmem_free(sql); return NULL; }

                // Use inline PRIMARY KEY only when this is the sole PK column.
                // Composite PKs get a table-level constraint appended after the loop.
                const char *pk  = (c->is_pk && npks == 1) ? " PRIMARY KEY" : "";
                const char *sep = (i > 0) ? ", " : "";
                char *tmp = dbmem_mprintf("%s%s%s%s", sql, sep, col, pk);
                dbmem_free(col);
                dbmem_free(sql);
                if (!tmp) return NULL;
                sql = tmp;
            }

            // Emit the table-level PRIMARY KEY constraint for composite PKs.
            if (npks > 1) {
                char *tmp = dbmem_mprintf("%s, PRIMARY KEY (", sql);
                dbmem_free(sql);
                if (!tmp) return NULL;
                sql = tmp;

                bool first_pk = true;
                for (int i = 0; i < desc->ncolumns; i++) {
                    if (!desc->columns[i].is_pk) continue;
                    const char *sep = first_pk ? "" : ", ";
                    first_pk = false;
                    tmp = dbmem_mprintf("%s%s\"%w\"", sql, sep, desc->columns[i].name);
                    dbmem_free(sql);
                    if (!tmp) return NULL;
                    sql = tmp;
                }
                tmp = dbmem_mprintf("%s)", sql);
                dbmem_free(sql);
                if (!tmp) return NULL;
                sql = tmp;
            }

            char *final = dbmem_mprintf("%s);", sql);
            dbmem_free(sql);
            return final;
        }

        case CLOUDSYNC_MIGRATION_DROP_TABLE: {
            if (!bare_table) return NULL;
            return dbmem_mprintf("DROP TABLE IF EXISTS \"%w\";", bare_table);
        }

        case CLOUDSYNC_MIGRATION_RENAME_TABLE: {
            if (!bare_table || !bare_new_name) return NULL;
            return dbmem_mprintf("ALTER TABLE \"%w\" RENAME TO \"%w\";",
                                 bare_table, bare_new_name);
        }

        case CLOUDSYNC_MIGRATION_CREATE_INDEX: {
            if (!desc->index_name || !bare_table || desc->nindex_columns == 0)
                return NULL;
            const char *unique_kw = desc->index_unique ? "UNIQUE " : "";
            char *sql = dbmem_mprintf("CREATE %sINDEX IF NOT EXISTS \"%w\" ON \"%w\" (",
                                      unique_kw, desc->index_name, bare_table);
            if (!sql) return NULL;
            for (int i = 0; i < desc->nindex_columns; i++) {
                const char *sep = (i > 0) ? ", " : "";
                char *tmp = dbmem_mprintf("%s%s\"%w\"", sql, sep, desc->index_columns[i]);
                dbmem_free(sql);
                if (!tmp) return NULL;
                sql = tmp;
            }
            char *final = dbmem_mprintf("%s);", sql);
            dbmem_free(sql);
            return final;
        }

        case CLOUDSYNC_MIGRATION_DROP_INDEX: {
            if (!desc->index_name) return NULL;
            return dbmem_mprintf("DROP INDEX IF EXISTS \"%w\";", desc->index_name);
        }

        case CLOUDSYNC_MIGRATION_INIT_SYNC:
            // Handled in migration.c via cloudsync_init_table(); no DDL here.
            return NULL;

        case CLOUDSYNC_MIGRATION_CUSTOM:
            if (!desc->sql_sqlite) return NULL;
            return dbmem_mprintf("%s", desc->sql_sqlite);

        default:
            return NULL;
    }
}

// MARK: - Execution -

int database_migration_execute(cloudsync_context *ctx,
                                cloudsync_migration_descriptor *desc) {
    if (!ctx || !desc) return DBRES_MISUSE;

    // INIT_SYNC is handled by the caller (migration.c).
    if (desc->op == CLOUDSYNC_MIGRATION_INIT_SYNC) return DBRES_OK;

    // SET_DEFAULT is not supported by SQLite ALTER TABLE.
    if (desc->op == CLOUDSYNC_MIGRATION_SET_DEFAULT) return DBRES_ERROR;

    char *sql = database_migration_sql(desc);
    if (!sql) return DBRES_ERROR;

    int rc = database_exec(ctx, sql);
    dbmem_free(sql);
    return rc;
}
