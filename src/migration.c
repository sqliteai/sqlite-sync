//
//  migration.c
//  cloudsync
//
//  Schema migration payload application.
//

#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cloudsync.h"
#include "database.h"
#include "dbutils.h"
#include "utils.h"

#define JSMN_STATIC
#include "jsmn.h"

#define CLOUDSYNC_MIGRATION_INITIAL_TOKENS 512
#define CLOUDSYNC_MIGRATION_SAVEPOINT  "cloudsync_migration"
#define CLOUDSYNC_ALTER_APPLY_SAVEPOINT "cloudsync_alter_apply"

typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
} migration_buffer;

typedef struct {
    const char *json;
    jsmntok_t *tokens;
    int ntokens;
} migration_json;

typedef enum {
    PENDING_ALTER_CREATE_TABLE,
    PENDING_ALTER_ADD_COLUMN,
    PENDING_ALTER_ADD_PRIMARY_KEY,
    PENDING_ALTER_AUGMENT_TABLE,
    PENDING_ALTER_SET_BLOCK_LWW,
    PENDING_ALTER_SET_COLUMN,
    PENDING_ALTER_SET_FILTER,
    PENDING_ALTER_DROP_COLUMN,
    PENDING_ALTER_RENAME_COLUMN,
    PENDING_ALTER_RAW_SQL
} pending_alter_kind;

typedef struct pending_alter_op {
    pending_alter_kind kind;
    char *table;
    char *a;
    char *b;
    char *c;
    char *d;
    bool flag;
    int64_t number;
    bool has_default;
    char *default_kind;
    char *default_value;
    char *sqlite_type_sql;
    char *sqlite_default_sql;
    char *postgresql_type_sql;
    char *postgresql_default_sql;
    char *sqlite_filter_sql;
    char *postgresql_filter_sql;
    struct pending_alter_op *next;
} pending_alter_op;

typedef struct pending_alter_context {
    cloudsync_context *data;
    pending_alter_op *ops;
    struct pending_alter_context *next;
} pending_alter_context;

static pending_alter_context *g_pending_alters = NULL;

static bool migration_sql_has_statement(const char *sql);
static bool migration_sql_has_transaction_control(const char *sql);

static bool migration_json_is_whitespace (char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool migration_json_has_single_root (const char *json, size_t json_len, jsmntok_t *tokens, int ntokens) {
    if (!json || !tokens || ntokens < 1 || tokens[0].start < 0 || tokens[0].end < tokens[0].start) return false;

    size_t cursor = 0;
    while (cursor < json_len && migration_json_is_whitespace(json[cursor])) cursor++;
    if (cursor != (size_t)tokens[0].start) return false;

    cursor = (size_t)tokens[0].end;
    while (cursor < json_len && migration_json_is_whitespace(json[cursor])) cursor++;
    return cursor == json_len;
}

static int migration_json_parse_root_object (const char *json, size_t json_len, jsmntok_t *tokens, unsigned int max_tokens) {
    if (!json || json_len == 0 || !tokens || max_tokens == 0) return JSMN_ERROR_INVAL;

    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, json, json_len, tokens, max_tokens);
    if (ntokens < 0) return ntokens;
    if (ntokens < 1 || tokens[0].type != JSMN_OBJECT || !migration_json_has_single_root(json, json_len, tokens, ntokens)) {
        return JSMN_ERROR_INVAL;
    }
    return ntokens;
}

static int migration_json_parse_root_object_alloc (const char *json, size_t json_len, jsmntok_t **tokens_out, int *ntokens_out) {
    if (tokens_out) *tokens_out = NULL;
    if (ntokens_out) *ntokens_out = 0;
    if (!json || json_len == 0 || !tokens_out || !ntokens_out) return DBRES_MISUSE;

    size_t max_tokens = json_len + 1;
    if (max_tokens < CLOUDSYNC_MIGRATION_INITIAL_TOKENS) max_tokens = CLOUDSYNC_MIGRATION_INITIAL_TOKENS;
    if (max_tokens > (size_t)INT_MAX) max_tokens = (size_t)INT_MAX;

    size_t cap = CLOUDSYNC_MIGRATION_INITIAL_TOKENS;
    if (cap > max_tokens) cap = max_tokens;
    while (cap > 0 && cap <= max_tokens) {
        if (cap > SIZE_MAX / sizeof(jsmntok_t)) return DBRES_NOMEM;
        jsmntok_t *tokens = cloudsync_memory_alloc(cap * sizeof(jsmntok_t));
        if (!tokens) return DBRES_NOMEM;

        int ntokens = migration_json_parse_root_object(json, json_len, tokens, (unsigned int)cap);
        if (ntokens == JSMN_ERROR_NOMEM) {
            cloudsync_memory_free(tokens);
            if (cap == max_tokens) break;
            size_t next = cap * 2;
            if (next <= cap || next > max_tokens) next = max_tokens;
            cap = next;
            continue;
        }
        if (ntokens < 1) {
            cloudsync_memory_free(tokens);
            return DBRES_MISUSE;
        }

        *tokens_out = tokens;
        *ntokens_out = ntokens;
        return DBRES_OK;
    }

    return DBRES_MISUSE;
}

static int migration_token_skip (migration_json *doc, int index) {
    if (!doc || index < 0 || index >= doc->ntokens) return index + 1;
    jsmntok_t *tok = &doc->tokens[index];
    int next = index + 1;
    if (tok->type == JSMN_OBJECT) {
        for (int i = 0; i < tok->size; ++i) {
            next = migration_token_skip(doc, next); // key
            next = migration_token_skip(doc, next); // value
        }
    } else if (tok->type == JSMN_ARRAY) {
        for (int i = 0; i < tok->size; ++i) {
            next = migration_token_skip(doc, next);
        }
    }
    return next;
}

static bool migration_token_eq (migration_json *doc, int index, const char *value) {
    if (!doc || index < 0 || index >= doc->ntokens || !value) return false;
    jsmntok_t *tok = &doc->tokens[index];
    int len = tok->end - tok->start;
    return tok->type == JSMN_STRING && (int)strlen(value) == len && strncmp(doc->json + tok->start, value, (size_t)len) == 0;
}

static int migration_object_get (migration_json *doc, int object_index, const char *key) {
    if (!doc || object_index < 0 || object_index >= doc->ntokens || !key) return -1;
    jsmntok_t *obj = &doc->tokens[object_index];
    if (obj->type != JSMN_OBJECT) return -1;

    int cursor = object_index + 1;
    for (int i = 0; i < obj->size; ++i) {
        int key_index = cursor;
        int value_index = migration_token_skip(doc, key_index);
        if (migration_token_eq(doc, key_index, key)) return value_index;
        cursor = migration_token_skip(doc, value_index);
    }
    return -1;
}

static int migration_array_item (migration_json *doc, int array_index, int item) {
    if (!doc || array_index < 0 || array_index >= doc->ntokens || item < 0) return -1;
    jsmntok_t *array = &doc->tokens[array_index];
    if (array->type != JSMN_ARRAY || item >= array->size) return -1;

    int cursor = array_index + 1;
    for (int i = 0; i < item; ++i) cursor = migration_token_skip(doc, cursor);
    return cursor;
}

static char *migration_token_dup (migration_json *doc, int index) {
    if (!doc || index < 0 || index >= doc->ntokens) return NULL;
    jsmntok_t *tok = &doc->tokens[index];
    if (tok->start < 0 || tok->end < tok->start) return NULL;
    int len = tok->end - tok->start;
    if (tok->type != JSMN_STRING) return cloudsync_string_ndup(doc->json + tok->start, (size_t)len);

    char *out = cloudsync_memory_zeroalloc((uint64_t)len + 1);
    if (!out) return NULL;
    int j = 0;
    const char *src = doc->json + tok->start;
    for (int i = 0; i < len; ) {
        if (src[i] == '\\' && i + 1 < len) {
            char c = src[i + 1];
            if (c == '"' || c == '\\' || c == '/') { out[j++] = c; i += 2; }
            else if (c == 'n') { out[j++] = '\n'; i += 2; }
            else if (c == 'r') { out[j++] = '\r'; i += 2; }
            else if (c == 't') { out[j++] = '\t'; i += 2; }
            else if (c == 'b') { out[j++] = '\b'; i += 2; }
            else if (c == 'f') { out[j++] = '\f'; i += 2; }
            else {
                out[j++] = src[i++];
            }
        } else {
            out[j++] = src[i++];
        }
    }
    out[j] = '\0';
    return out;
}

static char *migration_object_string (migration_json *doc, int object_index, const char *key) {
    int index = migration_object_get(doc, object_index, key);
    if (index < 0 || doc->tokens[index].type != JSMN_STRING) return NULL;
    return migration_token_dup(doc, index);
}

static int64_t migration_object_int (migration_json *doc, int object_index, const char *key, int64_t default_value) {
    int index = migration_object_get(doc, object_index, key);
    if (index < 0 || doc->tokens[index].type != JSMN_PRIMITIVE) return default_value;
    char *value = migration_token_dup(doc, index);
    if (!value) return default_value;
    int64_t result = strtoll(value, NULL, 10);
    cloudsync_memory_free(value);
    return result;
}

static bool migration_object_bool (migration_json *doc, int object_index, const char *key, bool default_value) {
    int index = migration_object_get(doc, object_index, key);
    if (index < 0 || doc->tokens[index].type != JSMN_PRIMITIVE) return default_value;
    jsmntok_t *tok = &doc->tokens[index];
    int len = tok->end - tok->start;
    if (len == 4 && strncmp(doc->json + tok->start, "true", 4) == 0) return true;
    if (len == 5 && strncmp(doc->json + tok->start, "false", 5) == 0) return false;
    return default_value;
}

static bool migration_object_bool_strict (migration_json *doc, int object_index, const char *key, bool default_value, bool *ok) {
    int index = migration_object_get(doc, object_index, key);
    if (index < 0) return default_value;
    if (index >= doc->ntokens || doc->tokens[index].type != JSMN_PRIMITIVE) {
        if (ok) *ok = false;
        return default_value;
    }
    jsmntok_t *tok = &doc->tokens[index];
    int len = tok->end - tok->start;
    if (len == 4 && strncmp(doc->json + tok->start, "true", 4) == 0) return true;
    if (len == 5 && strncmp(doc->json + tok->start, "false", 5) == 0) return false;
    if (ok) *ok = false;
    return default_value;
}

static bool migration_buffer_reserve (migration_buffer *buffer, size_t extra) {
    if (buffer->len + extra + 1 <= buffer->cap) return true;
    size_t cap = buffer->cap ? buffer->cap * 2 : 256;
    while (cap < buffer->len + extra + 1) cap *= 2;
    char *ptr = cloudsync_memory_realloc(buffer->ptr, cap);
    if (!ptr) return false;
    buffer->ptr = ptr;
    buffer->cap = cap;
    return true;
}

static bool migration_buffer_append_len (migration_buffer *buffer, const char *value, size_t len) {
    if (!migration_buffer_reserve(buffer, len)) return false;
    memcpy(buffer->ptr + buffer->len, value, len);
    buffer->len += len;
    buffer->ptr[buffer->len] = '\0';
    return true;
}

static bool migration_buffer_append (migration_buffer *buffer, const char *value) {
    return migration_buffer_append_len(buffer, value, strlen(value));
}

static bool migration_buffer_appendf (migration_buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    if (!migration_buffer_reserve(buffer, (size_t)needed)) {
        va_end(args);
        return false;
    }
    vsnprintf(buffer->ptr + buffer->len, buffer->cap - buffer->len, format, args);
    va_end(args);
    buffer->len += (size_t)needed;
    return true;
}

static char *migration_sql_quote_identifier (const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value);
    char *out = cloudsync_memory_alloc(len * 2 + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; ++i) {
        if (value[i] == '"') out[j++] = '"';
        out[j++] = value[i];
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static char *migration_sql_quote_literal (const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value);
    char *out = cloudsync_memory_alloc(len * 2 + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; ++i) {
        if (value[i] == '\'') out[j++] = '\'';
        out[j++] = value[i];
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

static bool migration_json_append_string (migration_buffer *buffer, const char *value) {
    if (!migration_buffer_append(buffer, "\"")) return false;
    if (value) {
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
            char tmp[8];
            switch (*p) {
                case '"': if (!migration_buffer_append(buffer, "\\\"")) return false; break;
                case '\\': if (!migration_buffer_append(buffer, "\\\\")) return false; break;
                case '\b': if (!migration_buffer_append(buffer, "\\b")) return false; break;
                case '\f': if (!migration_buffer_append(buffer, "\\f")) return false; break;
                case '\n': if (!migration_buffer_append(buffer, "\\n")) return false; break;
                case '\r': if (!migration_buffer_append(buffer, "\\r")) return false; break;
                case '\t': if (!migration_buffer_append(buffer, "\\t")) return false; break;
                default:
                    if (*p < 0x20) {
                        snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                        if (!migration_buffer_append(buffer, tmp)) return false;
                    } else {
                        if (!migration_buffer_append_len(buffer, (const char *)p, 1)) return false;
                    }
                    break;
            }
        }
    }
    return migration_buffer_append(buffer, "\"");
}

static char *migration_table_ref (cloudsync_context *data, const char *table_name) {
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    const char *schema = cloudsync_schema(data);
    return database_build_base_ref(schema, table_name);
#else
    (void)data;
    return migration_sql_quote_identifier(table_name);
#endif
}

static bool migration_blocks_table_exists (cloudsync_context *data, const char *table_name) {
    if (!table_name) return false;
    char *blocks_name = cloudsync_memory_mprintf("%s_cloudsync_blocks", table_name);
    if (!blocks_name) return false;
    bool exists = database_table_exists(data, blocks_name, cloudsync_schema(data));
    cloudsync_memory_free(blocks_name);
    return exists;
}

static int migration_delete_dropped_column_sync_metadata (cloudsync_context *data, const char *table, const char *column) {
    char *table_lit = migration_sql_quote_literal(table);
    char *column_lit = migration_sql_quote_literal(column);
    if (!table_lit || !column_lit) {
        if (table_lit) cloudsync_memory_free(table_lit);
        if (column_lit) cloudsync_memory_free(column_lit);
        return DBRES_NOMEM;
    }

    int rc = DBRES_OK;
    if (database_internal_table_exists(data, CLOUDSYNC_TABLE_SETTINGS_NAME)) {
        char *sql = cloudsync_memory_mprintf(
            "DELETE FROM cloudsync_table_settings WHERE tbl_name = %s AND col_name = %s;",
            table_lit, column_lit);
        if (!sql) { rc = DBRES_NOMEM; goto cleanup; }
        rc = database_exec(data, sql);
        cloudsync_memory_free(sql);
        if (rc != DBRES_OK) goto cleanup;
    }

    if (migration_blocks_table_exists(data, table)) {
        char *blocks_ref = database_build_blocks_ref(cloudsync_schema(data), table);
        char *like_pattern = block_build_colname(column, "%");
        char *like_lit = like_pattern ? migration_sql_quote_literal(like_pattern) : NULL;
        if (!blocks_ref || !like_pattern || !like_lit) {
            if (blocks_ref) cloudsync_memory_free(blocks_ref);
            if (like_pattern) cloudsync_memory_free(like_pattern);
            if (like_lit) cloudsync_memory_free(like_lit);
            rc = DBRES_NOMEM;
            goto cleanup;
        }

        char *sql = cloudsync_memory_mprintf("DELETE FROM %s WHERE col_name LIKE %s;", blocks_ref, like_lit);
        if (sql) {
            rc = database_exec(data, sql);
            cloudsync_memory_free(sql);
        } else {
            rc = DBRES_NOMEM;
        }

        cloudsync_memory_free(blocks_ref);
        cloudsync_memory_free(like_pattern);
        cloudsync_memory_free(like_lit);
        if (rc != DBRES_OK) goto cleanup;
    }

cleanup:
    cloudsync_memory_free(table_lit);
    cloudsync_memory_free(column_lit);
    return rc;
}

static char *migration_sqlite_or_pg_type (const char *logical_type) {
    if (!logical_type) logical_type = "text";
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    if (strcasecmp(logical_type, "uuid") == 0) return cloudsync_string_dup("UUID");
    if (strcasecmp(logical_type, "integer") == 0) return cloudsync_string_dup("BIGINT");
    if (strcasecmp(logical_type, "real") == 0) return cloudsync_string_dup("DOUBLE PRECISION");
    if (strcasecmp(logical_type, "numeric") == 0) return cloudsync_string_dup("NUMERIC");
    if (strcasecmp(logical_type, "blob") == 0) return cloudsync_string_dup("BYTEA");
    if (strcasecmp(logical_type, "boolean") == 0) return cloudsync_string_dup("BOOLEAN");
    if (strcasecmp(logical_type, "json") == 0) return cloudsync_string_dup("JSONB");
    if (strcasecmp(logical_type, "timestamp") == 0) return cloudsync_string_dup("TIMESTAMPTZ");
    return cloudsync_string_dup("TEXT");
#else
    if (strcasecmp(logical_type, "integer") == 0) return cloudsync_string_dup("INTEGER");
    if (strcasecmp(logical_type, "real") == 0) return cloudsync_string_dup("REAL");
    if (strcasecmp(logical_type, "numeric") == 0) return cloudsync_string_dup("NUMERIC");
    if (strcasecmp(logical_type, "blob") == 0) return cloudsync_string_dup("BLOB");
    if (strcasecmp(logical_type, "boolean") == 0) return cloudsync_string_dup("INTEGER");
    return cloudsync_string_dup("TEXT");
#endif
}

static char *migration_default_sql (migration_json *doc, int default_index) {
    if (default_index < 0) return NULL;
    jsmntok_t *tok = &doc->tokens[default_index];
    if (tok->type == JSMN_OBJECT) {
        char *type = migration_object_string(doc, default_index, "type");
        int value_index = migration_object_get(doc, default_index, "value");
        char *value = value_index >= 0 ? migration_token_dup(doc, value_index) : NULL;
        char *result = NULL;
        if (type && strcasecmp(type, "null") == 0) {
            result = cloudsync_string_dup("NULL");
        } else if (type && strcasecmp(type, "integer") == 0) {
            result = value ? cloudsync_string_dup(value) : cloudsync_string_dup("0");
        } else if (type && strcasecmp(type, "real") == 0) {
            result = value ? cloudsync_string_dup(value) : cloudsync_string_dup("0");
        } else if (type && strcasecmp(type, "boolean") == 0) {
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
            result = value && (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? cloudsync_string_dup("TRUE") : cloudsync_string_dup("FALSE");
#else
            result = value && (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? cloudsync_string_dup("1") : cloudsync_string_dup("0");
#endif
        } else if (type && strcasecmp(type, "blob") == 0) {
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
            result = cloudsync_memory_mprintf("decode('%s', 'hex')", value ? value : "");
#else
            result = cloudsync_memory_mprintf("X'%s'", value ? value : "");
#endif
        } else {
            result = migration_sql_quote_literal(value ? value : "");
        }
        if (type) cloudsync_memory_free(type);
        if (value) cloudsync_memory_free(value);
        return result;
    }
    if (tok->type == JSMN_STRING) {
        char *value = migration_token_dup(doc, default_index);
        char *result = migration_sql_quote_literal(value ? value : "");
        if (value) cloudsync_memory_free(value);
        return result;
    }
    if (tok->type == JSMN_PRIMITIVE) {
        char *value = migration_token_dup(doc, default_index);
        if (!value) return NULL;
        if (strcmp(value, "null") == 0) {
            cloudsync_memory_free(value);
            return cloudsync_string_dup("NULL");
        }
        return value;
    }
    return NULL;
}

static int migration_current_dialect_object (migration_json *doc, int object_index) {
    int dialects = migration_object_get(doc, object_index, "dialects");
    if (dialects < 0 || doc->tokens[dialects].type != JSMN_OBJECT) return -1;
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    return migration_object_get(doc, dialects, "postgresql");
#else
    return migration_object_get(doc, dialects, "sqlite");
#endif
}

static char *migration_column_definition (migration_json *doc, int col_index, bool allow_primary_key, bool *primary_key, bool *valid) {
    if (primary_key) *primary_key = false;
    if (valid) *valid = true;
    char *name = migration_object_string(doc, col_index, "name");
    char *logical_type = migration_object_string(doc, col_index, "type");
    char *qname = migration_sql_quote_identifier(name);
    bool bool_ok = true;
    bool nullable = migration_object_bool_strict(doc, col_index, "nullable", true, &bool_ok);
    bool pk_value = migration_object_bool_strict(doc, col_index, "primaryKey", false, &bool_ok);
    bool pk = allow_primary_key && pk_value;
    int default_index = migration_object_get(doc, col_index, "default");
    int dialect_index = migration_current_dialect_object(doc, col_index);
    char *sql_type = NULL;
    char *default_sql = NULL;
    if (!bool_ok) {
        if (valid) *valid = false;
        if (name) cloudsync_memory_free(name);
        if (logical_type) cloudsync_memory_free(logical_type);
        if (qname) cloudsync_memory_free(qname);
        return NULL;
    }
    if (dialect_index >= 0 && doc->tokens[dialect_index].type == JSMN_OBJECT) {
        sql_type = migration_object_string(doc, dialect_index, "typeSql");
        default_sql = migration_object_string(doc, dialect_index, "defaultSql");
    }
    if (!sql_type) sql_type = migration_sqlite_or_pg_type(logical_type);
    if (!default_sql) default_sql = migration_default_sql(doc, default_index);

    migration_buffer buffer = {0};
    if (qname && sql_type) {
        migration_buffer_appendf(&buffer, "%s %s", qname, sql_type);
        if (!nullable || pk) migration_buffer_append(&buffer, " NOT NULL");
        if (default_sql) migration_buffer_appendf(&buffer, " DEFAULT %s", default_sql);
    }

    if (primary_key) *primary_key = pk;
    if (name) cloudsync_memory_free(name);
    if (logical_type) cloudsync_memory_free(logical_type);
    if (qname) cloudsync_memory_free(qname);
    if (sql_type) cloudsync_memory_free(sql_type);
    if (default_sql) cloudsync_memory_free(default_sql);
    return buffer.ptr;
}

static int migration_create_migrations_table (cloudsync_context *data) {
    return database_exec(data,
        "CREATE TABLE IF NOT EXISTS cloudsync_migrations ("
        "migration_id TEXT PRIMARY KEY NOT NULL,"
        "schema_epoch INTEGER NOT NULL DEFAULT 0,"
        "target_schema_hash TEXT,"
        "applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");");
}

static bool migration_already_applied (cloudsync_context *data, const char *migration_id) {
    if (!migration_id) return false;
    char *literal = migration_sql_quote_literal(migration_id);
    if (!literal) return false;
    char *sql = cloudsync_memory_mprintf("SELECT COUNT(*) FROM cloudsync_migrations WHERE migration_id = %s;", literal);
    cloudsync_memory_free(literal);
    if (!sql) return false;
    int64_t count = 0;
    int rc = database_select_int(data, sql, &count);
    cloudsync_memory_free(sql);
    return rc == DBRES_OK && count > 0;
}

static int migration_record_applied (cloudsync_context *data, const char *migration_id, int64_t schema_epoch, const char *target_hash) {
    if (!migration_id) return DBRES_OK;
    char *id_lit = migration_sql_quote_literal(migration_id);
    char *hash_lit = target_hash ? migration_sql_quote_literal(target_hash) : cloudsync_string_dup("NULL");
    if (!id_lit || !hash_lit) {
        if (id_lit) cloudsync_memory_free(id_lit);
        if (hash_lit) cloudsync_memory_free(hash_lit);
        return DBRES_NOMEM;
    }
    char *sql = cloudsync_memory_mprintf(
        "INSERT INTO cloudsync_migrations (migration_id, schema_epoch, target_schema_hash) "
        "VALUES (%s, %lld, %s);",
        id_lit, (long long)schema_epoch, hash_lit);
    cloudsync_memory_free(id_lit);
    cloudsync_memory_free(hash_lit);
    if (!sql) return DBRES_NOMEM;
    int rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

static int migration_apply_raw_sql_text (cloudsync_context *data, const char *sql) {
    if (!sql || !migration_sql_has_statement(sql)) {
        return cloudsync_set_error(data, "rawSql requires SQL", DBRES_MISUSE);
    }
    if (migration_sql_has_transaction_control(sql)) {
        return cloudsync_set_error(data, "rawSql cannot contain transaction control statements", DBRES_MISUSE);
    }
    return database_exec(data, sql);
}

static int migration_apply_raw_sql (cloudsync_context *data, migration_json *doc, int op_index) {
    int sql_index = migration_object_get(doc, op_index, "sql");
    if (sql_index < 0) return cloudsync_set_error(data, "rawSql requires sql", DBRES_MISUSE);

    int dialect_index = sql_index;
    if (doc->tokens[sql_index].type == JSMN_OBJECT) {
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
        dialect_index = migration_object_get(doc, sql_index, "postgresql");
#else
        dialect_index = migration_object_get(doc, sql_index, "sqlite");
#endif
        if (dialect_index < 0) {
            if (migration_object_bool(doc, op_index, "skipMissingDialect", false)) return DBRES_OK;
            return cloudsync_set_error(data, "rawSql does not include SQL for this database dialect", DBRES_MISUSE);
        }
    }

    if (doc->tokens[dialect_index].type == JSMN_STRING) {
        char *sql = migration_token_dup(doc, dialect_index);
        if (!sql) return DBRES_NOMEM;
        int rc = migration_apply_raw_sql_text(data, sql);
        cloudsync_memory_free(sql);
        return rc;
    }

    if (doc->tokens[dialect_index].type == JSMN_ARRAY) {
        int count = doc->tokens[dialect_index].size;
        for (int i = 0; i < count; ++i) {
            int item = migration_array_item(doc, dialect_index, i);
            if (item < 0 || doc->tokens[item].type != JSMN_STRING) return cloudsync_set_error(data, "rawSql array items must be strings", DBRES_MISUSE);
            char *sql = migration_token_dup(doc, item);
            if (!sql) return DBRES_NOMEM;
            int rc = migration_apply_raw_sql_text(data, sql);
            cloudsync_memory_free(sql);
            if (rc != DBRES_OK) return rc;
        }
        return DBRES_OK;
    }

    return cloudsync_set_error(data, "rawSql sql must be a string, array, or dialect object", DBRES_MISUSE);
}

static int migration_apply_create_table (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    int cols_index = migration_object_get(doc, op_index, "columns");
    if (!table || cols_index < 0 || doc->tokens[cols_index].type != JSMN_ARRAY) {
        if (table) cloudsync_memory_free(table);
        return cloudsync_set_error(data, "createTable requires table and columns", DBRES_MISUSE);
    }

    if (database_table_exists(data, table, cloudsync_schema(data))) {
        cloudsync_memory_free(table);
        return cloudsync_set_error(data, "createTable target table already exists", DBRES_MISUSE);
    }

    char *table_ref = migration_table_ref(data, table);
    if (!table_ref) {
        cloudsync_memory_free(table);
        return DBRES_NOMEM;
    }
    migration_buffer sql = {0};
    migration_buffer pk = {0};
    int ncols = doc->tokens[cols_index].size;
    if (!migration_buffer_appendf(&sql, "CREATE TABLE %s (", table_ref)) {
        cloudsync_memory_free(table);
        cloudsync_memory_free(table_ref);
        return DBRES_NOMEM;
    }

    for (int i = 0; i < ncols; ++i) {
        int col_index = migration_array_item(doc, cols_index, i);
        bool is_pk = false;
        bool valid_column = true;
        char *def = migration_column_definition(doc, col_index, true, &is_pk, &valid_column);
        char *name = migration_object_string(doc, col_index, "name");
        char *qname = migration_sql_quote_identifier(name);
        if (!valid_column) {
            if (name) cloudsync_memory_free(name);
            if (qname) cloudsync_memory_free(qname);
            if (table) cloudsync_memory_free(table);
            if (table_ref) cloudsync_memory_free(table_ref);
            if (sql.ptr) cloudsync_memory_free(sql.ptr);
            if (pk.ptr) cloudsync_memory_free(pk.ptr);
            return cloudsync_set_error(data, "Column nullable and primaryKey fields must be boolean values", DBRES_MISUSE);
        }
        if (!def || !qname) {
            if (def) cloudsync_memory_free(def);
            if (name) cloudsync_memory_free(name);
            if (qname) cloudsync_memory_free(qname);
            if (table) cloudsync_memory_free(table);
            if (table_ref) cloudsync_memory_free(table_ref);
            if (sql.ptr) cloudsync_memory_free(sql.ptr);
            if (pk.ptr) cloudsync_memory_free(pk.ptr);
            return DBRES_NOMEM;
        }
        if (i > 0) migration_buffer_append(&sql, ", ");
        migration_buffer_append(&sql, def);
        if (is_pk) {
            if (pk.len > 0) migration_buffer_append(&pk, ", ");
            migration_buffer_append(&pk, qname);
        }
        cloudsync_memory_free(def);
        cloudsync_memory_free(name);
        cloudsync_memory_free(qname);
    }

    if (pk.len > 0) migration_buffer_appendf(&sql, ", PRIMARY KEY (%s)", pk.ptr);
    migration_buffer_append(&sql, ");");

    int rc = database_exec(data, sql.ptr);
    if (table) cloudsync_memory_free(table);
    if (table_ref) cloudsync_memory_free(table_ref);
    if (sql.ptr) cloudsync_memory_free(sql.ptr);
    if (pk.ptr) cloudsync_memory_free(pk.ptr);
    return rc;
}

static int migration_apply_add_column (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    int col_index = migration_object_get(doc, op_index, "column");
    if (!table || col_index < 0 || doc->tokens[col_index].type != JSMN_OBJECT) {
        if (table) cloudsync_memory_free(table);
        return cloudsync_set_error(data, "addColumn requires table and column", DBRES_MISUSE);
    }

    char *table_ref = migration_table_ref(data, table);
    bool valid_column = true;
    char *def = migration_column_definition(doc, col_index, false, NULL, &valid_column);
    if (!valid_column) {
        if (table) cloudsync_memory_free(table);
        if (table_ref) cloudsync_memory_free(table_ref);
        return cloudsync_set_error(data, "Column nullable and primaryKey fields must be boolean values", DBRES_MISUSE);
    }
    if (!table_ref || !def) {
        if (table) cloudsync_memory_free(table);
        if (table_ref) cloudsync_memory_free(table_ref);
        if (def) cloudsync_memory_free(def);
        return DBRES_NOMEM;
    }

    bool augmented = table_lookup(data, table) != NULL;
    int rc = DBRES_OK;
    if (augmented) {
        rc = cloudsync_begin_alter(data, table);
        if (rc != DBRES_OK) goto cleanup;
    }

    char *sql = cloudsync_memory_mprintf("ALTER TABLE %s ADD COLUMN %s;", table_ref, def);
    if (!sql) { rc = DBRES_NOMEM; goto cleanup; }
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;

    if (augmented) rc = cloudsync_commit_alter(data, table);

cleanup:
    cloudsync_memory_free(table);
    cloudsync_memory_free(table_ref);
    cloudsync_memory_free(def);
    return rc;
}

static int migration_apply_augment_table (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *algo = migration_object_string(doc, op_index, "algorithm");
    int64_t init_flags = migration_object_int(doc, op_index, "initFlags", 0);
    if (!table) return cloudsync_set_error(data, "augmentTable requires table", DBRES_MISUSE);
    int rc = cloudsync_init_table(data, table, algo ? algo : CLOUDSYNC_DEFAULT_ALGO, (CLOUDSYNC_INIT_FLAG)init_flags);
    cloudsync_memory_free(table);
    if (algo) cloudsync_memory_free(algo);
    return rc;
}

static int migration_apply_set_block_lww (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *column = migration_object_string(doc, op_index, "column");
    char *delimiter = migration_object_string(doc, op_index, "delimiter");
    if (!table || !column) {
        if (table) cloudsync_memory_free(table);
        if (column) cloudsync_memory_free(column);
        if (delimiter) cloudsync_memory_free(delimiter);
        return cloudsync_set_error(data, "setBlockLww requires table and column", DBRES_MISUSE);
    }
    int rc = cloudsync_setup_block_column(data, table, column, delimiter, true);
    cloudsync_memory_free(table);
    cloudsync_memory_free(column);
    if (delimiter) cloudsync_memory_free(delimiter);
    return rc;
}

static int migration_apply_set_column (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *column = migration_object_string(doc, op_index, "column");
    char *key = migration_object_string(doc, op_index, "key");
    char *value = migration_object_string(doc, op_index, "value");
    if (!table || !column || !key) {
        if (table) cloudsync_memory_free(table);
        if (column) cloudsync_memory_free(column);
        if (key) cloudsync_memory_free(key);
        if (value) cloudsync_memory_free(value);
        return cloudsync_set_error(data, "setColumn requires table, column, and key", DBRES_MISUSE);
    }
    int rc;
    if (value && strcmp(key, "algo") == 0 && strcmp(value, "block") == 0) {
        rc = cloudsync_setup_block_column(data, table, column, NULL, true);
    } else {
        rc = dbutils_table_settings_set_key_value(data, table, column, key, value);
    }
    cloudsync_memory_free(table);
    cloudsync_memory_free(column);
    cloudsync_memory_free(key);
    if (value) cloudsync_memory_free(value);
    return rc;
}

static int migration_apply_set_filter (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *filter = migration_object_string(doc, op_index, "filter");
    if (!filter) {
        int filters = migration_object_get(doc, op_index, "filters");
        if (filters >= 0 && doc->tokens[filters].type == JSMN_OBJECT) {
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
            int dialect_filter = migration_object_get(doc, filters, "postgresql");
#else
            int dialect_filter = migration_object_get(doc, filters, "sqlite");
#endif
            if (dialect_filter >= 0 && doc->tokens[dialect_filter].type == JSMN_STRING) {
                filter = migration_token_dup(doc, dialect_filter);
            }
        }
    }
    if (!table || !filter) {
        if (table) cloudsync_memory_free(table);
        if (filter) cloudsync_memory_free(filter);
        return cloudsync_set_error(data, "setFilter requires table and filter", DBRES_MISUSE);
    }
    if (!table_lookup(data, table)) {
        cloudsync_memory_free(table);
        cloudsync_memory_free(filter);
        return cloudsync_set_error(data, "setFilter table is not configured for sync", DBRES_MISUSE);
    }

    int rc = dbutils_table_settings_set_key_value(data, table, "*", "filter", filter);
    if (rc == DBRES_OK) {
        table_algo algo = dbutils_table_settings_get_algo(data, table);
        if (algo == table_algo_none) algo = table_algo_crdt_cls;
        rc = database_delete_triggers(data, table);
        if (rc == DBRES_OK) rc = database_create_triggers(data, table, algo, filter);
        if (rc == DBRES_OK) rc = cloudsync_reset_metatable(data, table);
    }

    cloudsync_memory_free(table);
    cloudsync_memory_free(filter);
    return rc;
}

static int migration_apply_drop_column (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *column = migration_object_string(doc, op_index, "column");
    if (!table || !column) {
        if (table) cloudsync_memory_free(table);
        if (column) cloudsync_memory_free(column);
        return cloudsync_set_error(data, "dropColumn requires table and column", DBRES_MISUSE);
    }
    char *table_ref = migration_table_ref(data, table);
    char *qcol = migration_sql_quote_identifier(column);
    if (!table_ref || !qcol) {
        if (table) cloudsync_memory_free(table);
        if (column) cloudsync_memory_free(column);
        if (table_ref) cloudsync_memory_free(table_ref);
        if (qcol) cloudsync_memory_free(qcol);
        return DBRES_NOMEM;
    }

    bool augmented = table_lookup(data, table) != NULL;
    int rc = DBRES_OK;
    if (augmented) {
        rc = cloudsync_begin_alter(data, table);
        if (rc != DBRES_OK) goto cleanup;
    }
    char *sql = cloudsync_memory_mprintf("ALTER TABLE %s DROP COLUMN %s;", table_ref, qcol);
    if (!sql) { rc = DBRES_NOMEM; goto cleanup; }
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;
    if (augmented) {
        rc = migration_delete_dropped_column_sync_metadata(data, table, column);
        if (rc == DBRES_OK) rc = cloudsync_commit_alter(data, table);
    }

cleanup:
    cloudsync_memory_free(table);
    cloudsync_memory_free(column);
    cloudsync_memory_free(table_ref);
    cloudsync_memory_free(qcol);
    return rc;
}

static int migration_update_renamed_column_metadata (cloudsync_context *data, const char *table, const char *from, const char *to) {
    char *meta_ref = database_build_meta_ref(cloudsync_schema(data), table);
    char *blocks_ref = database_build_blocks_ref(cloudsync_schema(data), table);
    char *from_lit = migration_sql_quote_literal(from);
    char *to_lit = migration_sql_quote_literal(to);
    if (!meta_ref || !blocks_ref || !from_lit || !to_lit) {
        if (meta_ref) cloudsync_memory_free(meta_ref);
        if (blocks_ref) cloudsync_memory_free(blocks_ref);
        if (from_lit) cloudsync_memory_free(from_lit);
        if (to_lit) cloudsync_memory_free(to_lit);
        return DBRES_NOMEM;
    }

#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    char *expr = cloudsync_memory_mprintf(
        "CASE WHEN col_name = %s THEN %s "
        "WHEN col_name LIKE (%s || chr(31) || '%%') THEN %s || substring(col_name from char_length(%s) + 1) "
        "ELSE col_name END",
        from_lit, to_lit, from_lit, to_lit, from_lit);
    char *where = cloudsync_memory_mprintf("col_name = %s OR col_name LIKE (%s || chr(31) || '%%')", from_lit, from_lit);
#else
    char *expr = cloudsync_memory_mprintf(
        "CASE WHEN col_name = %s THEN %s "
        "WHEN col_name LIKE (%s || char(31) || '%%') THEN %s || substr(col_name, length(%s) + 1) "
        "ELSE col_name END",
        from_lit, to_lit, from_lit, to_lit, from_lit);
    char *where = cloudsync_memory_mprintf("col_name = %s OR col_name LIKE (%s || char(31) || '%%')", from_lit, from_lit);
#endif
    int rc = DBRES_NOMEM;
    if (expr && where) {
        char *sql = cloudsync_memory_mprintf("UPDATE %s SET col_name = %s WHERE %s;", meta_ref, expr, where);
        if (sql) {
            rc = database_exec(data, sql);
            cloudsync_memory_free(sql);
        }
        if (rc == DBRES_OK && migration_blocks_table_exists(data, table)) {
            sql = cloudsync_memory_mprintf("UPDATE %s SET col_name = %s WHERE %s;", blocks_ref, expr, where);
            if (sql) {
                rc = database_exec(data, sql);
                cloudsync_memory_free(sql);
            }
        }
    }
    if (expr) cloudsync_memory_free(expr);
    if (where) cloudsync_memory_free(where);
    cloudsync_memory_free(meta_ref);
    cloudsync_memory_free(blocks_ref);
    cloudsync_memory_free(from_lit);
    cloudsync_memory_free(to_lit);
    return rc;
}

static int migration_apply_rename_column (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *from = migration_object_string(doc, op_index, "from");
    char *to = migration_object_string(doc, op_index, "to");
    if (!table || !from || !to) {
        if (table) cloudsync_memory_free(table);
        if (from) cloudsync_memory_free(from);
        if (to) cloudsync_memory_free(to);
        return cloudsync_set_error(data, "renameColumn requires table, from, and to", DBRES_MISUSE);
    }
    char *table_ref = migration_table_ref(data, table);
    char *qfrom = migration_sql_quote_identifier(from);
    char *qto = migration_sql_quote_identifier(to);
    if (!table_ref || !qfrom || !qto) {
        if (table) cloudsync_memory_free(table);
        if (from) cloudsync_memory_free(from);
        if (to) cloudsync_memory_free(to);
        if (table_ref) cloudsync_memory_free(table_ref);
        if (qfrom) cloudsync_memory_free(qfrom);
        if (qto) cloudsync_memory_free(qto);
        return DBRES_NOMEM;
    }

    bool augmented = table_lookup(data, table) != NULL;
    int rc = DBRES_OK;
    if (augmented) {
        rc = cloudsync_begin_alter(data, table);
        if (rc != DBRES_OK) goto cleanup;
    }

    char *sql = cloudsync_memory_mprintf("ALTER TABLE %s RENAME COLUMN %s TO %s;", table_ref, qfrom, qto);
    if (!sql) { rc = DBRES_NOMEM; goto cleanup; }
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    if (rc != DBRES_OK) goto cleanup;

    if (augmented) {
        rc = migration_update_renamed_column_metadata(data, table, from, to);
        if (rc == DBRES_OK) {
            char *from_lit = migration_sql_quote_literal(from);
            char *to_lit = migration_sql_quote_literal(to);
            char *table_lit = migration_sql_quote_literal(table);
            if (!from_lit || !to_lit || !table_lit) rc = DBRES_NOMEM;
            else {
                sql = cloudsync_memory_mprintf("UPDATE cloudsync_table_settings SET col_name = %s WHERE tbl_name = %s AND col_name = %s;", to_lit, table_lit, from_lit);
                if (sql) {
                    rc = database_exec(data, sql);
                    cloudsync_memory_free(sql);
                } else rc = DBRES_NOMEM;
            }
            if (from_lit) cloudsync_memory_free(from_lit);
            if (to_lit) cloudsync_memory_free(to_lit);
            if (table_lit) cloudsync_memory_free(table_lit);
        }
        if (rc == DBRES_OK) rc = cloudsync_commit_alter(data, table);
    }

cleanup:
    cloudsync_memory_free(table);
    cloudsync_memory_free(from);
    cloudsync_memory_free(to);
    cloudsync_memory_free(table_ref);
    cloudsync_memory_free(qfrom);
    cloudsync_memory_free(qto);
    return rc;
}

static int migration_apply_rebuild_table_sync (cloudsync_context *data, migration_json *doc, int op_index) {
    char *table = migration_object_string(doc, op_index, "table");
    char *algo = migration_object_string(doc, op_index, "algorithm");
    int64_t init_flags = migration_object_int(doc, op_index, "initFlags", 0);
    if (!table) return cloudsync_set_error(data, "rebuildTableSync requires table", DBRES_MISUSE);

    int ddl_index = migration_object_get(doc, op_index, "ddl");
    if (ddl_index >= 0 && doc->tokens[ddl_index].type != JSMN_ARRAY) {
        cloudsync_memory_free(table);
        if (algo) cloudsync_memory_free(algo);
        return cloudsync_set_error(data, "rebuildTableSync ddl must be an array", DBRES_MISUSE);
    }

    int block_index = migration_object_get(doc, op_index, "blockLww");
    if (block_index >= 0 && doc->tokens[block_index].type != JSMN_ARRAY) {
        cloudsync_memory_free(table);
        if (algo) cloudsync_memory_free(algo);
        return cloudsync_set_error(data, "rebuildTableSync blockLww must be an array", DBRES_MISUSE);
    }

    if (table_lookup(data, table)) {
        int rc = cloudsync_cleanup(data, table, true);
        if (rc != DBRES_OK) {
            cloudsync_memory_free(table);
            if (algo) cloudsync_memory_free(algo);
            return rc;
        }
    }

    if (ddl_index >= 0) {
        int count = doc->tokens[ddl_index].size;
        for (int i = 0; i < count; ++i) {
            int item = migration_array_item(doc, ddl_index, i);
            char *op = migration_object_string(doc, item, "op");
            int rc = DBRES_OK;
            if (op && strcmp(op, "rawSql") == 0) rc = migration_apply_raw_sql(data, doc, item);
            else rc = cloudsync_set_error(data, "rebuildTableSync ddl currently supports rawSql operations only", DBRES_MISUSE);
            if (op) cloudsync_memory_free(op);
            if (rc != DBRES_OK) {
                cloudsync_memory_free(table);
                if (algo) cloudsync_memory_free(algo);
                return rc;
            }
        }
    }

    int rc = cloudsync_init_table(data, table, algo ? algo : CLOUDSYNC_DEFAULT_ALGO, (CLOUDSYNC_INIT_FLAG)init_flags);
    if (rc != DBRES_OK) {
        cloudsync_memory_free(table);
        if (algo) cloudsync_memory_free(algo);
        return rc;
    }

    if (block_index >= 0 && doc->tokens[block_index].type == JSMN_ARRAY) {
        int count = doc->tokens[block_index].size;
        for (int i = 0; i < count; ++i) {
            int item = migration_array_item(doc, block_index, i);
            char *column = migration_object_string(doc, item, "column");
            char *delimiter = migration_object_string(doc, item, "delimiter");
            if (!column) rc = cloudsync_set_error(data, "rebuildTableSync blockLww requires column", DBRES_MISUSE);
            else rc = cloudsync_setup_block_column(data, table, column, delimiter, true);
            if (column) cloudsync_memory_free(column);
            if (delimiter) cloudsync_memory_free(delimiter);
            if (rc != DBRES_OK) break;
        }
    }

    cloudsync_memory_free(table);
    if (algo) cloudsync_memory_free(algo);
    return rc;
}

static int migration_apply_op (cloudsync_context *data, migration_json *doc, int op_index) {
    char *op = migration_object_string(doc, op_index, "op");
    if (!op) return cloudsync_set_error(data, "Migration operation requires op", DBRES_MISUSE);
    int rc = DBRES_MISUSE;
    if (strcmp(op, "createTable") == 0) rc = migration_apply_create_table(data, doc, op_index);
    else if (strcmp(op, "addColumn") == 0) rc = migration_apply_add_column(data, doc, op_index);
    else if (strcmp(op, "augmentTable") == 0) rc = migration_apply_augment_table(data, doc, op_index);
    else if (strcmp(op, "setBlockLww") == 0) rc = migration_apply_set_block_lww(data, doc, op_index);
    else if (strcmp(op, "setColumn") == 0) rc = migration_apply_set_column(data, doc, op_index);
    else if (strcmp(op, "setFilter") == 0) rc = migration_apply_set_filter(data, doc, op_index);
    else if (strcmp(op, "rawSql") == 0) rc = migration_apply_raw_sql(data, doc, op_index);
    else if (strcmp(op, "dropColumn") == 0) rc = migration_apply_drop_column(data, doc, op_index);
    else if (strcmp(op, "renameColumn") == 0) rc = migration_apply_rename_column(data, doc, op_index);
    else if (strcmp(op, "rebuildTableSync") == 0) rc = migration_apply_rebuild_table_sync(data, doc, op_index);
    else {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Unsupported migration operation %s", op);
        rc = cloudsync_set_error(data, buffer, DBRES_MISUSE);
    }
    cloudsync_memory_free(op);
    return rc;
}

int cloudsync_migration_apply (cloudsync_context *data, const char *payload, int payload_len, char **result_json) {
    if (result_json) *result_json = NULL;
    if (!data || !payload || payload_len <= 0) return cloudsync_set_error(data, "cloudsync_migration_apply requires a JSON payload", DBRES_MISUSE);

    jsmntok_t *tokens = NULL;
    int ntokens = 0;
    int parse_rc = migration_json_parse_root_object_alloc(payload, (size_t)payload_len, &tokens, &ntokens);
    if (parse_rc == DBRES_NOMEM) return DBRES_NOMEM;
    if (parse_rc != DBRES_OK) {
        return cloudsync_set_error(data, "Invalid migration JSON payload", DBRES_MISUSE);
    }

    migration_json doc = {payload, tokens, ntokens};
    char *migration_id = migration_object_string(&doc, 0, "migrationId");
    char *target_hash = migration_object_string(&doc, 0, "targetSchemaHash");
    char *computed_target_hash = NULL;
    char *base_hash = migration_object_string(&doc, 0, "baseSchemaHash");
    int64_t schema_epoch = migration_object_int(&doc, 0, "schemaEpoch", migration_object_int(&doc, 0, "targetSchemaEpoch", 0));
    int ops_index = migration_object_get(&doc, 0, "ops");
    if (ops_index < 0 || tokens[ops_index].type != JSMN_ARRAY) {
        if (migration_id) cloudsync_memory_free(migration_id);
        if (target_hash) cloudsync_memory_free(target_hash);
        if (base_hash) cloudsync_memory_free(base_hash);
        cloudsync_memory_free(tokens);
        return cloudsync_set_error(data, "Migration payload requires ops array", DBRES_MISUSE);
    }

    int rc = migration_create_migrations_table(data);
    if (rc != DBRES_OK) goto cleanup;

    if (migration_id && migration_already_applied(data, migration_id)) {
        if (result_json) *result_json = cloudsync_memory_mprintf("{\"status\":\"already_applied\",\"migrationId\":\"%s\"}", migration_id);
        rc = DBRES_OK;
        goto cleanup;
    }

    if (base_hash) {
        uint64_t current = database_schema_hash(data);
        uint64_t expected = (uint64_t)strtoull(base_hash, NULL, 10);
        if (current != expected) {
            rc = cloudsync_set_error(data, "Migration baseSchemaHash does not match the current schema hash", DBRES_MISUSE);
            goto cleanup;
        }
    }

    rc = database_begin_savepoint(data, CLOUDSYNC_MIGRATION_SAVEPOINT);
    if (rc != DBRES_OK) goto cleanup;

    int op_count = tokens[ops_index].size;
    for (int i = 0; i < op_count; ++i) {
        int op_index = migration_array_item(&doc, ops_index, i);
        rc = migration_apply_op(data, &doc, op_index);
        if (rc != DBRES_OK) break;
    }

    if (rc == DBRES_OK && cloudsync_config_exists(data) && dbutils_table_settings_count_tables(data) > 0) {
        cloudsync_update_schema_hash(data);
        uint64_t current = database_schema_hash(data);
        computed_target_hash = cloudsync_memory_mprintf("%" PRIu64, current);
        if (target_hash) {
            uint64_t expected = (uint64_t)strtoull(target_hash, NULL, 10);
            if (current != expected) {
                rc = cloudsync_set_error(data, "Migration targetSchemaHash does not match the computed schema hash", DBRES_MISUSE);
            }
        }
    }

    if (rc == DBRES_OK) rc = migration_record_applied(data, migration_id, schema_epoch, target_hash ? target_hash : computed_target_hash);

    if (rc == DBRES_OK) {
        rc = database_commit_savepoint(data, CLOUDSYNC_MIGRATION_SAVEPOINT);
        if (result_json && rc == DBRES_OK) {
            *result_json = cloudsync_memory_mprintf("{\"status\":\"applied\",\"migrationId\":%s%s%s}",
                migration_id ? "\"" : "null",
                migration_id ? migration_id : "",
                migration_id ? "\"" : "");
        }
    } else {
        database_rollback_savepoint(data, CLOUDSYNC_MIGRATION_SAVEPOINT);
        cloudsync_terminate(data);
    }

cleanup:
    if (migration_id) cloudsync_memory_free(migration_id);
    if (target_hash) cloudsync_memory_free(target_hash);
    if (computed_target_hash) cloudsync_memory_free(computed_target_hash);
    if (base_hash) cloudsync_memory_free(base_hash);
    cloudsync_memory_free(tokens);
    return rc;
}

// MARK: - Declarative alter builder -

static void pending_alter_op_free(pending_alter_op *op) {
    if (!op) return;
    if (op->table) cloudsync_memory_free(op->table);
    if (op->a) cloudsync_memory_free(op->a);
    if (op->b) cloudsync_memory_free(op->b);
    if (op->c) cloudsync_memory_free(op->c);
    if (op->d) cloudsync_memory_free(op->d);
    if (op->default_kind) cloudsync_memory_free(op->default_kind);
    if (op->default_value) cloudsync_memory_free(op->default_value);
    if (op->sqlite_type_sql) cloudsync_memory_free(op->sqlite_type_sql);
    if (op->sqlite_default_sql) cloudsync_memory_free(op->sqlite_default_sql);
    if (op->postgresql_type_sql) cloudsync_memory_free(op->postgresql_type_sql);
    if (op->postgresql_default_sql) cloudsync_memory_free(op->postgresql_default_sql);
    if (op->sqlite_filter_sql) cloudsync_memory_free(op->sqlite_filter_sql);
    if (op->postgresql_filter_sql) cloudsync_memory_free(op->postgresql_filter_sql);
    cloudsync_memory_free(op);
}

static pending_alter_context *pending_alter_context_for(cloudsync_context *data, bool create) {
    pending_alter_context *ctx = g_pending_alters;
    while (ctx) {
        if (ctx->data == data) return ctx;
        ctx = ctx->next;
    }
    if (!create) return NULL;
    ctx = cloudsync_memory_zeroalloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->data = data;
    ctx->next = g_pending_alters;
    g_pending_alters = ctx;
    return ctx;
}

static int pending_alter_append(cloudsync_context *data, pending_alter_op *op) {
    pending_alter_context *ctx = pending_alter_context_for(data, true);
    if (!ctx) {
        pending_alter_op_free(op);
        return DBRES_NOMEM;
    }
    if (!ctx->ops) ctx->ops = op;
    else {
        pending_alter_op *tail = ctx->ops;
        while (tail->next) tail = tail->next;
        tail->next = op;
    }
    return DBRES_OK;
}

static pending_alter_op *pending_alter_op_new(pending_alter_kind kind, const char *table) {
    pending_alter_op *op = cloudsync_memory_zeroalloc(sizeof(*op));
    if (!op) return NULL;
    op->kind = kind;
    if (table) {
        op->table = cloudsync_string_dup(table);
        if (!op->table) {
            pending_alter_op_free(op);
            return NULL;
        }
    }
    return op;
}

static bool pending_table_eq(pending_alter_op *op, const char *table) {
    return op && op->table && table && strcmp(op->table, table) == 0;
}

static pending_alter_op *pending_find_add_column(cloudsync_context *data, const char *table, const char *column) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    for (pending_alter_op *op = ctx ? ctx->ops : NULL; op; op = op->next) {
        if (op->kind == PENDING_ALTER_ADD_COLUMN && pending_table_eq(op, table) && op->a && column && strcmp(op->a, column) == 0) return op;
    }
    return NULL;
}

static bool pending_has_create_table(cloudsync_context *data, const char *table) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    for (pending_alter_op *op = ctx ? ctx->ops : NULL; op; op = op->next) {
        if (op->kind == PENDING_ALTER_CREATE_TABLE && pending_table_eq(op, table)) return true;
    }
    return false;
}

static bool pending_column_is_pk(cloudsync_context *data, const char *table, const char *column) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    for (pending_alter_op *op = ctx ? ctx->ops : NULL; op; op = op->next) {
        if (op->kind == PENDING_ALTER_ADD_PRIMARY_KEY && pending_table_eq(op, table) && op->a && column && strcmp(op->a, column) == 0) return true;
    }
    return false;
}

static bool pending_table_has_pk(cloudsync_context *data, const char *table) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    for (pending_alter_op *op = ctx ? ctx->ops : NULL; op; op = op->next) {
        if (op->kind == PENDING_ALTER_ADD_PRIMARY_KEY && pending_table_eq(op, table)) return true;
    }
    return false;
}

static bool pending_sql_default_is_non_null (const char *sql) {
    if (!sql) return false;
    while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r') sql++;
    if (strncasecmp(sql, "NULL", 4) == 0) {
        char next = sql[4];
        if (!((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9') || next == '_')) return false;
    }
    return true;
}

static bool pending_column_has_current_default(pending_alter_op *op) {
    if (!op) return false;
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    const char *sql = op->postgresql_default_sql;
#else
    const char *sql = op->sqlite_default_sql;
#endif
    if (sql) return pending_sql_default_is_non_null(sql);
    return op->has_default && op->default_kind && strcasecmp(op->default_kind, "null") != 0;
}

static bool pending_append_column_json(migration_buffer *buffer, cloudsync_context *data, pending_alter_op *op, bool include_pk) {
    bool pk = include_pk && pending_column_is_pk(data, op->table, op->a);
    if (!migration_buffer_append(buffer, "{")) return false;
    if (!migration_buffer_append(buffer, "\"name\":")) return false;
    if (!migration_json_append_string(buffer, op->a)) return false;
    if (!migration_buffer_append(buffer, ",\"type\":")) return false;
    if (!migration_json_append_string(buffer, op->b ? op->b : "text")) return false;
    if (!migration_buffer_appendf(buffer, ",\"nullable\":%s", op->flag ? "true" : "false")) return false;
    if (pk && !migration_buffer_append(buffer, ",\"primaryKey\":true")) return false;
    if (op->has_default) {
        if (!migration_buffer_append(buffer, ",\"default\":{\"type\":")) return false;
        if (!migration_json_append_string(buffer, op->default_kind ? op->default_kind : "text")) return false;
        if (op->default_value || (op->default_kind && strcasecmp(op->default_kind, "null") != 0)) {
            if (!migration_buffer_append(buffer, ",\"value\":")) return false;
            if (!migration_json_append_string(buffer, op->default_value ? op->default_value : "")) return false;
        }
        if (!migration_buffer_append(buffer, "}")) return false;
    }
    bool has_sqlite = op->sqlite_type_sql || op->sqlite_default_sql;
    bool has_pg = op->postgresql_type_sql || op->postgresql_default_sql;
    if (has_sqlite || has_pg) {
        bool first_dialect = true;
        if (!migration_buffer_append(buffer, ",\"dialects\":{")) return false;
        if (has_sqlite) {
            if (!migration_buffer_append(buffer, "\"sqlite\":{")) return false;
            bool first = true;
            if (op->sqlite_type_sql) {
                if (!migration_buffer_append(buffer, "\"typeSql\":")) return false;
                if (!migration_json_append_string(buffer, op->sqlite_type_sql)) return false;
                first = false;
            }
            if (op->sqlite_default_sql) {
                if (!first && !migration_buffer_append(buffer, ",")) return false;
                if (!migration_buffer_append(buffer, "\"defaultSql\":")) return false;
                if (!migration_json_append_string(buffer, op->sqlite_default_sql)) return false;
            }
            if (!migration_buffer_append(buffer, "}")) return false;
            first_dialect = false;
        }
        if (has_pg) {
            if (!first_dialect && !migration_buffer_append(buffer, ",")) return false;
            if (!migration_buffer_append(buffer, "\"postgresql\":{")) return false;
            bool first = true;
            if (op->postgresql_type_sql) {
                if (!migration_buffer_append(buffer, "\"typeSql\":")) return false;
                if (!migration_json_append_string(buffer, op->postgresql_type_sql)) return false;
                first = false;
            }
            if (op->postgresql_default_sql) {
                if (!first && !migration_buffer_append(buffer, ",")) return false;
                if (!migration_buffer_append(buffer, "\"defaultSql\":")) return false;
                if (!migration_json_append_string(buffer, op->postgresql_default_sql)) return false;
            }
            if (!migration_buffer_append(buffer, "}")) return false;
        }
        if (!migration_buffer_append(buffer, "}")) return false;
    }
    return migration_buffer_append(buffer, "}");
}

static int pending_validate_ops(cloudsync_context *data, bool *destructive) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    if (!ctx || !ctx->ops) return cloudsync_set_error(data, "No pending alter operations", DBRES_MISUSE);
    for (pending_alter_op *op = ctx->ops; op; op = op->next) {
        if (op->kind == PENDING_ALTER_RAW_SQL) {
            if (destructive) *destructive = true;
            continue;
        }
        if (!op->table || !op->table[0]) return cloudsync_set_error(data, "cloudsync alter operation requires table", DBRES_MISUSE);
        if (op->kind == PENDING_ALTER_DROP_COLUMN || op->kind == PENDING_ALTER_RENAME_COLUMN) {
            if (destructive) *destructive = true;
        }
        if (op->kind == PENDING_ALTER_ADD_COLUMN && !op->flag && !pending_column_has_current_default(op) && !pending_column_is_pk(data, op->table, op->a)) {
            return cloudsync_set_error(data, "NOT NULL columns require a default value unless they are primary keys", DBRES_MISUSE);
        }
        if (op->kind == PENDING_ALTER_ADD_PRIMARY_KEY && !pending_has_create_table(data, op->table)) {
            return cloudsync_set_error(data, "cloudsync_alter_add_primary_key is supported only while creating a table", DBRES_MISUSE);
        }
    }
    for (pending_alter_op *op = ctx->ops; op; op = op->next) {
        if (op->kind == PENDING_ALTER_CREATE_TABLE && !pending_table_has_pk(data, op->table)) {
            return cloudsync_set_error(data, "createTable requires at least one primary key", DBRES_MISUSE);
        }
    }
    return DBRES_OK;
}

static bool pending_append_set_filter_json(migration_buffer *buffer, pending_alter_op *op) {
    if (!migration_buffer_append(buffer, "{\"op\":\"setFilter\",\"table\":")) return false;
    if (!migration_json_append_string(buffer, op->table)) return false;
    if (op->a) {
        if (!migration_buffer_append(buffer, ",\"filter\":")) return false;
        if (!migration_json_append_string(buffer, op->a)) return false;
    }
    if (op->sqlite_filter_sql || op->postgresql_filter_sql) {
        bool first = true;
        if (!migration_buffer_append(buffer, ",\"filters\":{")) return false;
        if (op->sqlite_filter_sql) {
            if (!migration_buffer_append(buffer, "\"sqlite\":")) return false;
            if (!migration_json_append_string(buffer, op->sqlite_filter_sql)) return false;
            first = false;
        }
        if (op->postgresql_filter_sql) {
            if (!first && !migration_buffer_append(buffer, ",")) return false;
            if (!migration_buffer_append(buffer, "\"postgresql\":")) return false;
            if (!migration_json_append_string(buffer, op->postgresql_filter_sql)) return false;
        }
        if (!migration_buffer_append(buffer, "}")) return false;
    }
    return migration_buffer_append(buffer, "}");
}

static bool pending_append_raw_sql_json(migration_buffer *buffer, pending_alter_op *op) {
    if (!migration_buffer_append(buffer, "{\"op\":\"rawSql\",\"sql\":")) return false;
    if (op->b) {
        if (!migration_buffer_append(buffer, "{")) return false;
        if (!migration_json_append_string(buffer, op->b)) return false;
        if (!migration_buffer_append(buffer, ":[")) return false;
        if (!migration_json_append_string(buffer, op->a)) return false;
        if (!migration_buffer_append(buffer, "]},\"skipMissingDialect\":true")) return false;
    } else {
        if (!migration_json_append_string(buffer, op->a)) return false;
    }
    return migration_buffer_append(buffer, "}");
}

static char *pending_build_payload(cloudsync_context *data, const char *migration_id, const char *base_hash, const char *target_hash, bool *destructive_out) {
    bool destructive = false;
    int rc = pending_validate_ops(data, &destructive);
    if (rc != DBRES_OK) return NULL;
    if (destructive_out) *destructive_out = destructive;

    pending_alter_context *ctx = pending_alter_context_for(data, false);
    migration_buffer payload = {0};
    if (!migration_buffer_append(&payload, "{")) goto oom;
    if (!migration_buffer_append(&payload, "\"type\":\"cloudsync.schema.migration\"")) goto oom;
    if (!migration_buffer_appendf(&payload, ",\"formatVersion\":%d", destructive ? 2 : 1)) goto oom;
    if (migration_id) {
        if (!migration_buffer_append(&payload, ",\"migrationId\":")) goto oom;
        if (!migration_json_append_string(&payload, migration_id)) goto oom;
    }
    if (base_hash) {
        if (!migration_buffer_append(&payload, ",\"baseSchemaHash\":")) goto oom;
        if (!migration_json_append_string(&payload, base_hash)) goto oom;
    }
    if (target_hash) {
        if (!migration_buffer_append(&payload, ",\"targetSchemaHash\":")) goto oom;
        if (!migration_json_append_string(&payload, target_hash)) goto oom;
    }
    if (!migration_buffer_append(&payload, destructive ? ",\"requiredCapabilities\":[\"schema:write\",\"schema:destructive\"]" : ",\"requiredCapabilities\":[\"schema:write\"]")) goto oom;
    if (!migration_buffer_append(&payload, ",\"ops\":[")) goto oom;
    bool first_op = true;

    for (pending_alter_op *op = ctx->ops; op; op = op->next) {
        if (op->kind == PENDING_ALTER_ADD_PRIMARY_KEY) continue;
        if (op->kind == PENDING_ALTER_ADD_COLUMN && pending_has_create_table(data, op->table)) continue;
        if (!first_op && !migration_buffer_append(&payload, ",")) goto oom;
        switch (op->kind) {
            case PENDING_ALTER_CREATE_TABLE: {
                if (!migration_buffer_append(&payload, "{\"op\":\"createTable\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"columns\":[")) goto oom;
                bool first_col = true;
                for (pending_alter_op *col = ctx->ops; col; col = col->next) {
                    if (col->kind != PENDING_ALTER_ADD_COLUMN || !pending_table_eq(col, op->table)) continue;
                    if (!first_col && !migration_buffer_append(&payload, ",")) goto oom;
                    if (!pending_append_column_json(&payload, data, col, true)) goto oom;
                    first_col = false;
                }
                if (!migration_buffer_append(&payload, "]}")) goto oom;
                break;
            }
            case PENDING_ALTER_ADD_COLUMN:
                if (!migration_buffer_append(&payload, "{\"op\":\"addColumn\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"column\":")) goto oom;
                if (!pending_append_column_json(&payload, data, op, false)) goto oom;
                if (!migration_buffer_append(&payload, "}")) goto oom;
                break;
            case PENDING_ALTER_AUGMENT_TABLE:
                if (!migration_buffer_append(&payload, "{\"op\":\"augmentTable\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"algorithm\":")) goto oom;
                if (!migration_json_append_string(&payload, op->a ? op->a : CLOUDSYNC_DEFAULT_ALGO)) goto oom;
                if (!migration_buffer_appendf(&payload, ",\"initFlags\":%lld}", (long long)op->number)) goto oom;
                break;
            case PENDING_ALTER_SET_BLOCK_LWW:
                if (!migration_buffer_append(&payload, "{\"op\":\"setBlockLww\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"column\":")) goto oom;
                if (!migration_json_append_string(&payload, op->a)) goto oom;
                if (op->b) {
                    if (!migration_buffer_append(&payload, ",\"delimiter\":")) goto oom;
                    if (!migration_json_append_string(&payload, op->b)) goto oom;
                }
                if (!migration_buffer_append(&payload, "}")) goto oom;
                break;
            case PENDING_ALTER_SET_COLUMN:
                if (!migration_buffer_append(&payload, "{\"op\":\"setColumn\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"column\":")) goto oom;
                if (!migration_json_append_string(&payload, op->a)) goto oom;
                if (!migration_buffer_append(&payload, ",\"key\":")) goto oom;
                if (!migration_json_append_string(&payload, op->b)) goto oom;
                if (op->c) {
                    if (!migration_buffer_append(&payload, ",\"value\":")) goto oom;
                    if (!migration_json_append_string(&payload, op->c)) goto oom;
                }
                if (!migration_buffer_append(&payload, "}")) goto oom;
                break;
            case PENDING_ALTER_SET_FILTER:
                if (!pending_append_set_filter_json(&payload, op)) goto oom;
                break;
            case PENDING_ALTER_DROP_COLUMN:
                if (!migration_buffer_append(&payload, "{\"op\":\"dropColumn\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"column\":")) goto oom;
                if (!migration_json_append_string(&payload, op->a)) goto oom;
                if (!migration_buffer_append(&payload, "}")) goto oom;
                break;
            case PENDING_ALTER_RENAME_COLUMN:
                if (!migration_buffer_append(&payload, "{\"op\":\"renameColumn\",\"table\":")) goto oom;
                if (!migration_json_append_string(&payload, op->table)) goto oom;
                if (!migration_buffer_append(&payload, ",\"from\":")) goto oom;
                if (!migration_json_append_string(&payload, op->a)) goto oom;
                if (!migration_buffer_append(&payload, ",\"to\":")) goto oom;
                if (!migration_json_append_string(&payload, op->b)) goto oom;
                if (!migration_buffer_append(&payload, "}")) goto oom;
                break;
            case PENDING_ALTER_RAW_SQL:
                if (!pending_append_raw_sql_json(&payload, op)) goto oom;
                break;
            default:
                break;
        }
        first_op = false;
    }
    if (!migration_buffer_append(&payload, "]}")) goto oom;
    return payload.ptr;
oom:
    if (payload.ptr) cloudsync_memory_free(payload.ptr);
    cloudsync_set_error(data, "Unable to build pending alter payload", DBRES_NOMEM);
    return NULL;
}

static int pending_migration_table_create(cloudsync_context *data) {
    return database_exec(data,
        "CREATE TABLE IF NOT EXISTS cloudsync_pending_migration ("
        "migration_id TEXT PRIMARY KEY NOT NULL,"
        "table_name TEXT,"
        "payload TEXT NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "uploaded_at TEXT,"
        "last_error TEXT"
        ");");
}

static int pending_migration_save(cloudsync_context *data, const char *migration_id, const char *table, const char *payload) {
    int rc = pending_migration_table_create(data);
    if (rc != DBRES_OK) return rc;
    char *id_lit = migration_sql_quote_literal(migration_id);
    char *table_lit = table ? migration_sql_quote_literal(table) : cloudsync_string_dup("NULL");
    char *payload_lit = migration_sql_quote_literal(payload);
    if (!id_lit || !table_lit || !payload_lit) {
        if (id_lit) cloudsync_memory_free(id_lit);
        if (table_lit) cloudsync_memory_free(table_lit);
        if (payload_lit) cloudsync_memory_free(payload_lit);
        return DBRES_NOMEM;
    }
    char *sql = NULL;
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    sql = cloudsync_memory_mprintf(
        "INSERT INTO cloudsync_pending_migration (migration_id, table_name, payload, uploaded_at, last_error) "
        "VALUES (%s, %s, %s, NULL, NULL) "
        "ON CONFLICT (migration_id) DO UPDATE SET "
        "table_name = excluded.table_name, payload = excluded.payload, uploaded_at = NULL, last_error = NULL;",
        id_lit, table_lit, payload_lit);
#else
    sql = cloudsync_memory_mprintf(
        "INSERT OR REPLACE INTO cloudsync_pending_migration (migration_id, table_name, payload, uploaded_at, last_error) "
        "VALUES (%s, %s, %s, NULL, NULL);",
        id_lit, table_lit, payload_lit);
#endif
    cloudsync_memory_free(id_lit);
    cloudsync_memory_free(table_lit);
    cloudsync_memory_free(payload_lit);
    if (!sql) return DBRES_NOMEM;
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

int cloudsync_pending_migration_count(cloudsync_context *data) {
    if (!database_internal_table_exists(data, "cloudsync_pending_migration")) return 0;
    int64_t count = 0;
    int rc = database_select_int(data, "SELECT COUNT(*) FROM cloudsync_pending_migration WHERE uploaded_at IS NULL;", &count);
    return rc == DBRES_OK ? (int)count : 0;
}

char *cloudsync_pending_migration_next_id(cloudsync_context *data) {
    if (!database_internal_table_exists(data, "cloudsync_pending_migration")) return NULL;
    char *migration_id = NULL;
    int rc = database_select_text(data, "SELECT migration_id FROM cloudsync_pending_migration WHERE uploaded_at IS NULL ORDER BY created_at, migration_id LIMIT 1;", &migration_id);
    return rc == DBRES_OK ? migration_id : NULL;
}

char *cloudsync_pending_migration_payload(cloudsync_context *data, const char *migration_id) {
    if (!migration_id || !database_internal_table_exists(data, "cloudsync_pending_migration")) return NULL;
    char *id_lit = migration_sql_quote_literal(migration_id);
    if (!id_lit) return NULL;
    char *sql = cloudsync_memory_mprintf("SELECT payload FROM cloudsync_pending_migration WHERE migration_id = %s AND uploaded_at IS NULL;", id_lit);
    cloudsync_memory_free(id_lit);
    if (!sql) return NULL;
    char *payload = NULL;
    int rc = database_select_text(data, sql, &payload);
    cloudsync_memory_free(sql);
    return rc == DBRES_OK ? payload : NULL;
}

int cloudsync_pending_migration_mark_uploaded(cloudsync_context *data, const char *migration_id) {
    if (!migration_id) return DBRES_MISUSE;
    int rc = pending_migration_table_create(data);
    if (rc != DBRES_OK) return rc;
    char *id_lit = migration_sql_quote_literal(migration_id);
    if (!id_lit) return DBRES_NOMEM;
    char *sql = cloudsync_memory_mprintf("UPDATE cloudsync_pending_migration SET uploaded_at = CURRENT_TIMESTAMP, last_error = NULL WHERE migration_id = %s;", id_lit);
    cloudsync_memory_free(id_lit);
    if (!sql) return DBRES_NOMEM;
    rc = database_exec(data, sql);
    cloudsync_memory_free(sql);
    return rc;
}

static bool migration_sql_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool migration_sql_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool migration_sql_ident_char(char c) {
    return migration_sql_ident_start(c) || (c >= '0' && c <= '9') || c == '$';
}

static const char *migration_sql_skip_space_comments(const char *sql) {
    const char *p = sql;
    while (p && *p) {
        while (migration_sql_is_space(*p)) p++;
        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    return p;
}

static bool migration_sql_keyword_eq(const char *start, size_t len, const char *keyword) {
    return strlen(keyword) == len && strncasecmp(start, keyword, len) == 0;
}

static bool migration_sql_forbidden_transaction_keyword(const char *start, size_t len) {
    return migration_sql_keyword_eq(start, len, "begin") ||
           migration_sql_keyword_eq(start, len, "commit") ||
           migration_sql_keyword_eq(start, len, "end") ||
           migration_sql_keyword_eq(start, len, "abort") ||
           migration_sql_keyword_eq(start, len, "rollback") ||
           migration_sql_keyword_eq(start, len, "savepoint") ||
           migration_sql_keyword_eq(start, len, "release");
}

static const char *migration_sql_next_statement(const char *sql) {
    bool single_quote = false;
    bool double_quote = false;
    for (const char *p = sql; p && *p; ++p) {
        if (single_quote) {
            if (*p == '\'' && p[1] == '\'') p++;
            else if (*p == '\'') single_quote = false;
            continue;
        }
        if (double_quote) {
            if (*p == '"' && p[1] == '"') p++;
            else if (*p == '"') double_quote = false;
            continue;
        }
        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p && *p != '\n') p++;
            if (!*p) return p;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (!*p) return p;
            p++;
            continue;
        }
        if (*p == '$') {
            const char *tag_end = p + 1;
            while (migration_sql_ident_char(*tag_end)) tag_end++;
            if (*tag_end == '$') {
                size_t tag_len = (size_t)(tag_end - p) + 1;
                const char *q = tag_end + 1;
                while (*q && strncmp(q, p, tag_len) != 0) q++;
                if (!*q) return q;
                p = q + tag_len - 1;
                continue;
            }
        }
        if (*p == '\'') {
            single_quote = true;
            continue;
        }
        if (*p == '"') {
            double_quote = true;
            continue;
        }
        if (*p == ';') return p + 1;
    }
    return sql + strlen(sql);
}

static bool migration_sql_has_statement(const char *sql) {
    const char *p = migration_sql_skip_space_comments(sql);
    while (p && *p == ';') p = migration_sql_skip_space_comments(p + 1);
    return p && *p;
}

static bool migration_sql_has_transaction_control(const char *sql) {
    const char *p = sql;
    while (p && *p) {
        p = migration_sql_skip_space_comments(p);
        while (*p == ';') p = migration_sql_skip_space_comments(p + 1);
        if (!*p) return false;
        if (migration_sql_ident_start(*p)) {
            const char *start = p;
            while (migration_sql_ident_char(*p)) p++;
            if (migration_sql_forbidden_transaction_keyword(start, (size_t)(p - start))) return true;
        }
        p = migration_sql_next_statement(p);
    }
    return false;
}

static int cloudsync_alter_append_simple(cloudsync_context *data, pending_alter_kind kind, const char *table, const char *a, const char *b, const char *c) {
    if (!table || !table[0]) return cloudsync_set_error(data, "cloudsync alter requires table", DBRES_MISUSE);
    pending_alter_op *op = pending_alter_op_new(kind, table);
    if (!op) return DBRES_NOMEM;
    op->a = a ? cloudsync_string_dup(a) : NULL;
    op->b = b ? cloudsync_string_dup(b) : NULL;
    op->c = c ? cloudsync_string_dup(c) : NULL;
    return pending_alter_append(data, op);
}

int cloudsync_alter_create_table(cloudsync_context *data, const char *table) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_CREATE_TABLE, table, NULL, NULL, NULL);
}

int cloudsync_alter_add_column(cloudsync_context *data, const char *table, const char *column, const char *type, bool nullable, bool has_default, const char *default_value) {
    if (!table || !column || !type) return cloudsync_set_error(data, "cloudsync_alter_add_column requires table, column, and type", DBRES_MISUSE);
    pending_alter_op *op = pending_alter_op_new(PENDING_ALTER_ADD_COLUMN, table);
    if (!op) return DBRES_NOMEM;
    op->a = cloudsync_string_dup(column);
    op->b = cloudsync_string_dup(type);
    op->flag = nullable;
    op->has_default = has_default;
    op->default_kind = has_default ? cloudsync_string_dup(default_value ? type : "null") : NULL;
    op->default_value = (has_default && default_value) ? cloudsync_string_dup(default_value) : NULL;
    if (!op->a || !op->b || (has_default && !op->default_kind)) {
        pending_alter_op_free(op);
        return DBRES_NOMEM;
    }
    return pending_alter_append(data, op);
}

int cloudsync_alter_add_column_dialect(cloudsync_context *data, const char *table, const char *column, const char *dialect, const char *type_sql, bool nullable, bool has_default_sql, const char *default_sql) {
    if (!table || !column || !dialect || !type_sql) return cloudsync_set_error(data, "cloudsync_alter_add_column_dialect requires table, column, dialect, and type SQL", DBRES_MISUSE);
    pending_alter_op *op = pending_find_add_column(data, table, column);
    if (!op) return cloudsync_set_error(data, "Dialect override requires a pending addColumn operation", DBRES_MISUSE);
    char **type_slot = NULL;
    char **default_slot = NULL;
    if (strcasecmp(dialect, "sqlite") == 0) {
        type_slot = &op->sqlite_type_sql;
        default_slot = &op->sqlite_default_sql;
    } else if (strcasecmp(dialect, "postgresql") == 0) {
        type_slot = &op->postgresql_type_sql;
        default_slot = &op->postgresql_default_sql;
    } else {
        return cloudsync_set_error(data, "Unsupported dialect override", DBRES_MISUSE);
    }
    if (*type_slot) cloudsync_memory_free(*type_slot);
    *type_slot = cloudsync_string_dup(type_sql);
    if (*default_slot) {
        cloudsync_memory_free(*default_slot);
        *default_slot = NULL;
    }
    if (has_default_sql) {
        *default_slot = default_sql ? cloudsync_string_dup(default_sql) : cloudsync_string_dup("NULL");
    }
    (void)nullable;
    return *type_slot ? DBRES_OK : DBRES_NOMEM;
}

int cloudsync_alter_add_primary_key(cloudsync_context *data, const char *table, const char *column) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_ADD_PRIMARY_KEY, table, column, NULL, NULL);
}

int cloudsync_alter_augment_table(cloudsync_context *data, const char *table, const char *algorithm, int64_t init_flags) {
    if (!table || !table[0]) return cloudsync_set_error(data, "cloudsync_alter_augment_table requires table", DBRES_MISUSE);
    pending_alter_op *op = pending_alter_op_new(PENDING_ALTER_AUGMENT_TABLE, table);
    if (!op) return DBRES_NOMEM;
    op->a = cloudsync_string_dup(algorithm ? algorithm : CLOUDSYNC_DEFAULT_ALGO);
    op->number = init_flags;
    return pending_alter_append(data, op);
}

int cloudsync_alter_set_block_lww(cloudsync_context *data, const char *table, const char *column, const char *delimiter) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_SET_BLOCK_LWW, table, column, delimiter, NULL);
}

int cloudsync_alter_set_column(cloudsync_context *data, const char *table, const char *column, const char *key, const char *value) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_SET_COLUMN, table, column, key, value);
}

int cloudsync_alter_set_filter(cloudsync_context *data, const char *table, const char *filter) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_SET_FILTER, table, filter, NULL, NULL);
}

int cloudsync_alter_set_filter_dialect(cloudsync_context *data, const char *table, const char *dialect, const char *filter) {
    if (!table || !dialect || !filter) return cloudsync_set_error(data, "cloudsync_alter_set_filter_dialect requires table, dialect, and filter", DBRES_MISUSE);
    pending_alter_context *ctx = pending_alter_context_for(data, true);
    if (!ctx) return DBRES_NOMEM;
    pending_alter_op *op = NULL;
    for (pending_alter_op *cur = ctx->ops; cur; cur = cur->next) {
        if (cur->kind == PENDING_ALTER_SET_FILTER && pending_table_eq(cur, table)) op = cur;
    }
    if (!op) {
        op = pending_alter_op_new(PENDING_ALTER_SET_FILTER, table);
        if (!op) return DBRES_NOMEM;
        int rc = pending_alter_append(data, op);
        if (rc != DBRES_OK) return rc;
    }
    char **slot = NULL;
    if (strcasecmp(dialect, "sqlite") == 0) slot = &op->sqlite_filter_sql;
    else if (strcasecmp(dialect, "postgresql") == 0) slot = &op->postgresql_filter_sql;
    else return cloudsync_set_error(data, "Unsupported dialect override", DBRES_MISUSE);
    if (*slot) cloudsync_memory_free(*slot);
    *slot = cloudsync_string_dup(filter);
    return *slot ? DBRES_OK : DBRES_NOMEM;
}

int cloudsync_alter_drop_column(cloudsync_context *data, const char *table, const char *column) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_DROP_COLUMN, table, column, NULL, NULL);
}

int cloudsync_alter_rename_column(cloudsync_context *data, const char *table, const char *from, const char *to) {
    return cloudsync_alter_append_simple(data, PENDING_ALTER_RENAME_COLUMN, table, from, to, NULL);
}

static int cloudsync_alter_sql_internal(cloudsync_context *data, const char *sql, const char *dialect) {
    if (!sql || !migration_sql_has_statement(sql)) return cloudsync_set_error(data, "cloudsync_alter_sql requires SQL", DBRES_MISUSE);
    if (migration_sql_has_transaction_control(sql)) return cloudsync_set_error(data, "cloudsync_alter_sql cannot contain transaction control statements", DBRES_MISUSE);

    const char *canonical_dialect = NULL;
    if (dialect) {
        if (strcasecmp(dialect, "sqlite") == 0) canonical_dialect = "sqlite";
        else if (strcasecmp(dialect, "postgresql") == 0) canonical_dialect = "postgresql";
        else return cloudsync_set_error(data, "Unsupported SQL dialect", DBRES_MISUSE);
    }

    pending_alter_op *op = pending_alter_op_new(PENDING_ALTER_RAW_SQL, NULL);
    if (!op) return DBRES_NOMEM;
    op->a = cloudsync_string_dup(sql);
    op->b = canonical_dialect ? cloudsync_string_dup(canonical_dialect) : NULL;
    op->flag = canonical_dialect != NULL;
    if (!op->a || (canonical_dialect && !op->b)) {
        pending_alter_op_free(op);
        return DBRES_NOMEM;
    }
    return pending_alter_append(data, op);
}

int cloudsync_alter_sql(cloudsync_context *data, const char *sql) {
    return cloudsync_alter_sql_internal(data, sql, NULL);
}

int cloudsync_alter_sql_dialect(cloudsync_context *data, const char *dialect, const char *sql) {
    return cloudsync_alter_sql_internal(data, sql, dialect);
}

int cloudsync_alter_clear(cloudsync_context *data, const char *table) {
    pending_alter_context *ctx = pending_alter_context_for(data, false);
    if (!ctx) return DBRES_OK;
    pending_alter_op **link = &ctx->ops;
    while (*link) {
        pending_alter_op *op = *link;
        if (!table || pending_table_eq(op, table)) {
            *link = op->next;
            op->next = NULL;
            pending_alter_op_free(op);
        } else {
            link = &op->next;
        }
    }
    return DBRES_OK;
}

void cloudsync_alter_clear_context(cloudsync_context *data) {
    pending_alter_context **link = &g_pending_alters;
    while (*link) {
        pending_alter_context *ctx = *link;
        if (ctx->data == data) {
            *link = ctx->next;
            pending_alter_op *op = ctx->ops;
            while (op) {
                pending_alter_op *next = op->next;
                pending_alter_op_free(op);
                op = next;
            }
            cloudsync_memory_free(ctx);
            return;
        }
        link = &ctx->next;
    }
}

char *cloudsync_alter_preview(cloudsync_context *data) {
    char uuid[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(uuid, true);
    bool destructive = false;
    return pending_build_payload(data, uuid, NULL, NULL, &destructive);
}

int cloudsync_alter_apply(cloudsync_context *data, char **result_json) {
    if (result_json) *result_json = NULL;
    char uuid[UUID_STR_MAXLEN];
    cloudsync_uuid_v7_string(uuid, true);
    bool destructive = false;
    char *payload = pending_build_payload(data, uuid, NULL, NULL, &destructive);
    if (!payload) return cloudsync_errcode(data);

    char *apply_result = NULL;
    char *final_payload = NULL;
    bool savepoint_open = false;
    int rc = database_begin_savepoint(data, CLOUDSYNC_ALTER_APPLY_SAVEPOINT);
    if (rc != DBRES_OK) goto cleanup;
    savepoint_open = true;

    rc = cloudsync_migration_apply(data, payload, (int)strlen(payload), &apply_result);
    if (apply_result) {
        cloudsync_memory_free(apply_result);
        apply_result = NULL;
    }
    if (rc != DBRES_OK) goto rollback;

    final_payload = pending_build_payload(data, uuid, NULL, NULL, &destructive);
    if (!final_payload) {
        rc = cloudsync_errcode(data);
        if (rc == DBRES_OK) rc = DBRES_NOMEM;
        goto rollback;
    }

    rc = pending_migration_save(data, uuid, NULL, final_payload);
    if (rc != DBRES_OK) goto rollback;

    rc = database_commit_savepoint(data, CLOUDSYNC_ALTER_APPLY_SAVEPOINT);
    if (rc != DBRES_OK) goto rollback;
    savepoint_open = false;

    rc = cloudsync_alter_clear(data, NULL);
    if (result_json && rc == DBRES_OK) {
        *result_json = cloudsync_memory_mprintf("{\"status\":\"applied\",\"migrationId\":\"%s\",\"pendingUpload\":true}", uuid);
    }

cleanup:
    cloudsync_memory_free(payload);
    if (final_payload) cloudsync_memory_free(final_payload);
    return rc;

rollback:
    if (savepoint_open) {
        database_rollback_savepoint(data, CLOUDSYNC_ALTER_APPLY_SAVEPOINT);
        cloudsync_terminate(data);
    }
    goto cleanup;
}
