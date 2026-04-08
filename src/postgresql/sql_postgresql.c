//
//  sql_postgresql.c
//  cloudsync
//
//  PostgreSQL-specific SQL queries
//  Created by Claude Code on 22/12/25.
//

#include "../sql.h"

// MARK: Settings

const char * const SQL_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_settings WHERE key=$1;";

const char * const SQL_SETTINGS_SET_KEY_VALUE_REPLACE =
    "INSERT INTO cloudsync_settings (key, value) VALUES ($1, $2) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;";

const char * const SQL_SETTINGS_SET_KEY_VALUE_DELETE =
    "DELETE FROM cloudsync_settings WHERE key = $1;";

const char * const SQL_TABLE_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_table_settings WHERE (tbl_name=$1 AND col_name=$2 AND key=$3);";

const char * const SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE =
    "DELETE FROM cloudsync_table_settings WHERE tbl_name=$1;";

const char * const SQL_TABLE_SETTINGS_REPLACE =
    "INSERT INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES ($1, $2, $3, $4) "
    "ON CONFLICT (tbl_name, col_name, key) DO UPDATE SET value = EXCLUDED.value;";

const char * const SQL_TABLE_SETTINGS_DELETE_ONE =
    "DELETE FROM cloudsync_table_settings WHERE (tbl_name=$1 AND col_name=$2 AND key=$3);";

const char * const SQL_TABLE_SETTINGS_COUNT_TABLES =
    "SELECT count(*) FROM cloudsync_table_settings WHERE key='algo';";

const char * const SQL_SETTINGS_LOAD_GLOBAL =
    "SELECT key, value FROM cloudsync_settings;";

const char * const SQL_SETTINGS_LOAD_TABLE =
    "SELECT lower(tbl_name), lower(col_name), key, value FROM cloudsync_table_settings ORDER BY tbl_name, col_name;";

const char * const SQL_CREATE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_settings (key TEXT PRIMARY KEY NOT NULL, value TEXT);"
    "CREATE TABLE IF NOT EXISTS public.app_schema_version ("
    "version BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY"
    ");"
    "CREATE OR REPLACE FUNCTION bump_app_schema_version() "
    "RETURNS event_trigger AS $$ "
    "BEGIN "
    "INSERT INTO public.app_schema_version DEFAULT VALUES; "
    "END;"
    "$$ LANGUAGE plpgsql;"
    "DROP EVENT TRIGGER IF EXISTS app_schema_change;"
    "CREATE EVENT TRIGGER app_schema_change "
    "ON ddl_command_end "
    "EXECUTE FUNCTION bump_app_schema_version();";

// format strings (snprintf) are also static SQL templates
const char * const SQL_INSERT_SETTINGS_STR_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', '%s');";

const char * const SQL_INSERT_SETTINGS_INT_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', %lld);";

const char * const SQL_CREATE_SITE_ID_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_site_id ("
    "id BIGSERIAL PRIMARY KEY, "
    "site_id BYTEA UNIQUE NOT NULL"
    ");";

const char * const SQL_INSERT_SITE_ID_ROWID =
    "INSERT INTO cloudsync_site_id (id, site_id) VALUES ($1, $2);";

const char * const SQL_CREATE_TABLE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_table_settings (tbl_name TEXT NOT NULL, col_name TEXT NOT NULL, key TEXT NOT NULL, value TEXT, PRIMARY KEY(tbl_name,col_name,key));";

const char * const SQL_CREATE_SCHEMA_VERSIONS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (hash BIGINT PRIMARY KEY, seq INTEGER NOT NULL)";

const char * const SQL_SETTINGS_CLEANUP_DROP_ALL =
    "DROP TABLE IF EXISTS cloudsync_settings CASCADE; "
    "DROP TABLE IF EXISTS cloudsync_site_id CASCADE; "
    "DROP TABLE IF EXISTS cloudsync_table_settings CASCADE; "
    "DROP TABLE IF EXISTS cloudsync_schema_versions CASCADE;";

// MARK: CloudSync

const char * const SQL_DBVERSION_BUILD_QUERY =
    "WITH table_names AS ("
    "SELECT quote_ident(schemaname) || '.' || quote_ident(tablename) as tbl_name "
    "FROM pg_tables "
    "WHERE tablename LIKE '%_cloudsync'"
    "), "
    "query_parts AS ("
    "SELECT tbl_name, "
    "format('SELECT COALESCE(MAX(db_version), 0) FROM %s', tbl_name) as part "
    "FROM table_names"
    ") "
    "SELECT string_agg(part, ' UNION ALL ') FROM query_parts;";

const char * const SQL_CHANGES_INSERT_ROW =
    "INSERT INTO cloudsync_changes(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) "
    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9);";

// MARK: Additional SQL constants for PostgreSQL

const char * const SQL_SITEID_SELECT_ROWID0 =
    "SELECT site_id FROM cloudsync_site_id WHERE id = 0;";

const char * const SQL_DATA_VERSION =
    "SELECT txid_snapshot_xmin(txid_current_snapshot());";  // was "PRAGMA data_version"

const char * const SQL_SCHEMA_VERSION =
    "SELECT COALESCE(max(version), 0) FROM app_schema_version;";  // was "PRAGMA schema_version"

const char * const SQL_SITEID_GETSET_ROWID_BY_SITEID =
    "INSERT INTO cloudsync_site_id (site_id) VALUES ($1) "
    "ON CONFLICT(site_id) DO UPDATE SET site_id = EXCLUDED.site_id "
    "RETURNING id;";

const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID =
    "SELECT string_agg(quote_ident(column_name), ',' ORDER BY ordinal_position) "
    "FROM information_schema.columns "
    "WHERE table_name = $1 AND column_name NOT IN ("
    "SELECT column_name FROM information_schema.key_column_usage "
    "WHERE table_name = $1 AND constraint_name LIKE '%_pkey'"
    ");";  // TODO: build full SELECT ... WHERE ctid=? analog with ordered columns like SQLite

const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_PK =
    "WITH tbl AS ("
    "  SELECT to_regclass('%s') AS oid"
    "), "
    "pk AS ("
    "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_index x "
    "  JOIN tbl t ON t.oid = x.indrelid "
    "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
    "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
    "  WHERE x.indisprimary "
    "  ORDER BY k.ord"
    "), "
    "nonpk AS ("
    "  SELECT a.attname "
    "  FROM pg_attribute a "
    "  JOIN tbl t ON t.oid = a.attrelid "
    "  WHERE a.attnum > 0 AND NOT a.attisdropped "
    "    AND a.attnum NOT IN ("
    "      SELECT k.attnum "
    "      FROM pg_index x "
    "      JOIN tbl t2 ON t2.oid = x.indrelid "
    "      JOIN LATERAL unnest(x.indkey) AS k(attnum) ON true "
    "      WHERE x.indisprimary"
    "    ) "
    "  ORDER BY a.attnum"
    ") "
    "SELECT "
    "  'SELECT '"
    "  || (SELECT string_agg(format('%%I', attname), ',') FROM nonpk)"
    "  || ' FROM ' || (SELECT (oid::regclass)::text FROM tbl)"
    "  || ' WHERE '"
    "  || (SELECT string_agg(format('%%I=$%%s::%%s', attname, ord, coltype), ' AND ' ORDER BY ord) FROM pk)"
    "  || ';';";

const char * const SQL_DELETE_ROW_BY_ROWID =
    "DELETE FROM %s WHERE ctid = $1;";  // TODO: consider using PK-based deletion; ctid is unstable

const char * const SQL_BUILD_DELETE_ROW_BY_PK =
    "WITH tbl AS ("
    "  SELECT to_regclass('%s') AS oid"
    "), "
    "pk AS ("
    "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_index x "
    "  JOIN tbl t ON t.oid = x.indrelid "
    "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
    "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
    "  WHERE x.indisprimary "
    "  ORDER BY k.ord"
    ") "
    "SELECT "
    "  'DELETE FROM ' || (SELECT (oid::regclass)::text FROM tbl)"
    "  || ' WHERE '"
    "  || (SELECT string_agg(format('%%I=$%%s::%%s', attname, ord, coltype), ' AND ' ORDER BY ord) FROM pk)"
    "  || ';';";

const char * const SQL_INSERT_ROWID_IGNORE =
    "INSERT INTO %s DEFAULT VALUES ON CONFLICT DO NOTHING;";  // TODO: adapt to explicit PK inserts (no rowid in PG)

const char * const SQL_UPSERT_ROWID_AND_COL_BY_ROWID =
    "INSERT INTO %s (ctid, %s) VALUES ($1, $2) "
    "ON CONFLICT DO UPDATE SET %s = $2;";  // TODO: align with SQLite upsert by rowid; avoid ctid

const char * const SQL_BUILD_INSERT_PK_IGNORE =
    "WITH tbl AS ("
    "  SELECT to_regclass('%s') AS oid"
    "), "
    "pk AS ("
    "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_index x "
    "  JOIN tbl t ON t.oid = x.indrelid "
    "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
    "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
    "  WHERE x.indisprimary "
    "  ORDER BY k.ord"
    ") "
    "SELECT "
    "  'INSERT INTO ' || (SELECT (oid::regclass)::text FROM tbl)"
    "  || ' (' || (SELECT string_agg(format('%%I', attname), ',') FROM pk) || ')'"
    "  || ' VALUES (' || (SELECT string_agg(format('$%%s::%%s', ord, coltype), ',') FROM pk) || ')'"
    "  || ' ON CONFLICT DO NOTHING;';";

const char * const SQL_BUILD_UPSERT_PK_AND_COL =
    "WITH tbl AS ("
    "  SELECT to_regclass('%s') AS oid"
    "), "
    "pk AS ("
    "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_index x "
    "  JOIN tbl t ON t.oid = x.indrelid "
    "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
    "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
    "  WHERE x.indisprimary "
    "  ORDER BY k.ord"
    "), "
    "pk_count AS ("
    "  SELECT count(*) AS n FROM pk"
    "), "
    "col AS ("
    "  SELECT '%s'::text AS colname, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_attribute a "
    "  JOIN tbl t ON t.oid = a.attrelid "
    "  WHERE a.attname = '%s' AND a.attnum > 0 AND NOT a.attisdropped"
    ") "
    "SELECT "
    "  'INSERT INTO ' || (SELECT (oid::regclass)::text FROM tbl)"
    "  || ' (' || (SELECT string_agg(format('%%I', attname), ',') FROM pk)"
    "  || ',' || (SELECT format('%%I', colname) FROM col) || ')'"
    "  || ' VALUES (' || (SELECT string_agg(format('$%%s::%%s', ord, coltype), ',') FROM pk)"
    "  || ',' || (SELECT format('$%%s::%%s', (SELECT n FROM pk_count) + 1, coltype) FROM col) || ')'"
    "  || ' ON CONFLICT (' || (SELECT string_agg(format('%%I', attname), ',') FROM pk) || ')'"
    "  || ' DO UPDATE SET ' || (SELECT format('%%I', colname) FROM col)"
    "  || '=' || (SELECT format('$%%s::%%s', (SELECT n FROM pk_count) + 2, coltype) FROM col) || ';';";

const char * const SQL_SELECT_COLS_BY_ROWID_FMT =
    "SELECT %s%s%s FROM %s WHERE ctid = $1;";  // TODO: align with PK/rowid selection builder

const char * const SQL_BUILD_SELECT_COLS_BY_PK_FMT =
    "WITH tbl AS ("
    "  SELECT to_regclass('%s') AS tblreg"
    "), "
    "pk AS ("
    "  SELECT a.attname, k.ord, format_type(a.atttypid, a.atttypmod) AS coltype "
    "  FROM pg_index x "
    "  JOIN tbl t ON t.tblreg = x.indrelid "
    "  JOIN LATERAL unnest(x.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
    "  JOIN pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = k.attnum "
    "  WHERE x.indisprimary "
    "  ORDER BY k.ord"
    "), "
    "col AS ("
    "  SELECT '%s'::text AS colname"
    ") "
    "SELECT "
    "  'SELECT ' || (SELECT format('%%I', colname) FROM col) "
    "  || ' FROM ' || (SELECT tblreg::text FROM tbl)"
    "  || ' WHERE '"
    "  || (SELECT string_agg(format('%%I=$%%s::%%s', attname, ord, coltype), ' AND ' ORDER BY ord) FROM pk)"
    "  || ';';";

const char * const SQL_CLOUDSYNC_ROW_EXISTS_BY_PK =
    "SELECT EXISTS(SELECT 1 FROM %s WHERE pk = $1 LIMIT 1);";

const char * const SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION =
    "UPDATE %s "
    "SET col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, "
    "db_version = $1, seq = $2, site_id = 0 "
    "WHERE pk = $3 AND col_name = '%s';";

const char * const SQL_CLOUDSYNC_UPSERT_COL_INIT_OR_BUMP_VERSION =
    "INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, '%s', 1, $2, $3, 0) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = CASE %s.col_version %% 2 WHEN 0 THEN %s.col_version + 1 ELSE %s.col_version + 2 END, "
    "db_version = $2, seq = $3, site_id = 0;";

const char * const SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION =
    "INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, $2, $3, $4, $5, 0) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = %s.col_version + 1, db_version = $6, seq = $7, site_id = 0;";

const char * const SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL =
    "DELETE FROM %s WHERE pk = $1 AND col_name != '%s';";  // TODO: match SQLite delete semantics

const char * const SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL =
    "WITH moved AS ("
    "  SELECT col_name "
    "  FROM %s WHERE pk = $3 AND col_name != '%s'"
    "), "
    "upserted AS ("
    "  INSERT INTO %s (pk, col_name, col_version, db_version, seq, site_id) "
    "  SELECT $1, col_name, 1, $2, cloudsync_seq(), 0 "
    "  FROM moved "
    "  ON CONFLICT (pk, col_name) DO UPDATE SET "
    "  col_version = 1, db_version = $2, seq = cloudsync_seq(), site_id = 0"
    ") "
    "DELETE FROM %s WHERE pk = $3 AND col_name != '%s';";

const char * const SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS =
    "SELECT COALESCE("
    "(SELECT col_version FROM %s WHERE pk = $1 AND col_name = '%s'), "
    "(SELECT 1 FROM %s WHERE pk = $1 LIMIT 1)"
    ");";

const char * const SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID =
    "INSERT INTO %s "
    "(pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES ($1, $2, $3, cloudsync_db_version_next($4), $5, $6) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET "
    "col_version = EXCLUDED.col_version, "
    "db_version = cloudsync_db_version_next($4), "
    "seq = EXCLUDED.seq, "
    "site_id = EXCLUDED.site_id "
    "RETURNING ((db_version::bigint << 30) | seq);";  // TODO: align RETURNING and bump logic with SQLite (version increments on conflict)

const char * const SQL_CLOUDSYNC_TOMBSTONE_PK_EXCEPT_COL =
    "UPDATE %s "
    "SET col_version = 0, db_version = cloudsync_db_version_next($1) "
    "WHERE pk = $2 AND col_name != '%s';";  // TODO: confirm tombstone semantics match SQLite

const char * const SQL_CLOUDSYNC_SELECT_COL_VERSION_BY_PK_COL =
    "SELECT col_version FROM %s WHERE pk = $1 AND col_name = $2;";

const char * const SQL_CLOUDSYNC_SELECT_SITE_ID_BY_PK_COL =
    "SELECT site_id FROM %s WHERE pk = $1 AND col_name = $2;";

const char * const SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID =
    "SELECT c.column_name, c.ordinal_position "
    "FROM information_schema.columns c "
    "WHERE c.table_name = '%s' "
    "AND c.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
    "AND c.column_name NOT IN ("
    "  SELECT kcu.column_name FROM information_schema.table_constraints tc "
    "  JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name "
    "    AND tc.table_schema = kcu.table_schema "
    "  WHERE tc.table_name = '%s' AND tc.table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
    "  AND tc.constraint_type = 'PRIMARY KEY'"
    ") "
    "ORDER BY ordinal_position;";

const char * const SQL_DROP_CLOUDSYNC_TABLE =
    "DROP TABLE IF EXISTS %s CASCADE;";

const char * const SQL_DELETE_ALL_FROM_CLOUDSYNC_TABLE =
    "DELETE FROM %s;";

const char * const SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL =
    "DELETE FROM %s WHERE col_name NOT IN ("
    "SELECT column_name FROM information_schema.columns WHERE table_name = '%s' "
    "AND table_schema = COALESCE(NULLIF('%s', ''), current_schema()) "
    "UNION SELECT '%s'"
    ");";

const char * const SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT =
    "SELECT string_agg(quote_ident(column_name), ',' ORDER BY ordinal_position) "
    "FROM information_schema.key_column_usage "
    "WHERE table_name = '%s' AND table_schema = COALESCE(cloudsync_schema(), current_schema()) "
    "AND constraint_name LIKE '%%_pkey';";

const char * const SQL_CLOUDSYNC_GC_DELETE_ORPHANED_PK =
    "DELETE FROM %s "
    "WHERE (col_name != '%s' OR (col_name = '%s' AND col_version %% 2 != 0)) "
    "AND NOT EXISTS ("
    "SELECT 1 FROM %s "
    "WHERE %s.pk = cloudsync_pk_encode(%s) LIMIT 1"
    ");";

const char * const SQL_PRAGMA_TABLEINFO_PK_COLLIST =
    "SELECT string_agg(quote_ident(column_name), ',') "
    "FROM information_schema.key_column_usage "
    "WHERE table_name = '%s' AND table_schema = COALESCE(cloudsync_schema(), current_schema()) "
    "AND constraint_name LIKE '%%_pkey';";

const char * const SQL_PRAGMA_TABLEINFO_PK_DECODE_SELECTLIST =
    "SELECT string_agg("
    "'cloudsync_pk_decode(pk, ' || ordinal_position || ') AS ' || quote_ident(column_name), ',' ORDER BY ordinal_position"
    ") "
    "FROM information_schema.key_column_usage "
    "WHERE table_name = '%s' AND table_schema = COALESCE(cloudsync_schema(), current_schema()) "
    "AND constraint_name LIKE '%%_pkey';";

const char * const SQL_CLOUDSYNC_INSERT_MISSING_PKS_FROM_BASE_EXCEPT_SYNC =
    "SELECT cloudsync_insert('%s', %s) "
    "FROM (SELECT %s FROM %s EXCEPT SELECT %s FROM %s);";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM %s) "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM %s _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = $1"
    ");";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL_FILTERED =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM %s WHERE (%s)) "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM %s _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = $1"
    ");";

// MARK: Blocks (block-level LWW)

const char * const SQL_BLOCKS_CREATE_TABLE =
    "CREATE TABLE IF NOT EXISTS %s ("
    "pk BYTEA NOT NULL, "
    "col_name TEXT COLLATE \"C\" NOT NULL, "
    "col_value TEXT, "
    "PRIMARY KEY (pk, col_name))";

const char * const SQL_BLOCKS_UPSERT =
    "INSERT INTO %s (pk, col_name, col_value) VALUES ($1, $2, $3) "
    "ON CONFLICT (pk, col_name) DO UPDATE SET col_value = EXCLUDED.col_value";

const char * const SQL_BLOCKS_SELECT =
    "SELECT col_value FROM %s WHERE pk = $1 AND col_name = $2";

const char * const SQL_BLOCKS_DELETE =
    "DELETE FROM %s WHERE pk = $1 AND col_name = $2";

const char * const SQL_BLOCKS_LIST_ALIVE =
    "SELECT b.col_value FROM %s b "
    "JOIN %s m ON b.pk = m.pk AND b.col_name = m.col_name "
    "WHERE b.pk = $1 AND b.col_name LIKE $2 "
    "AND m.pk = $3 AND m.col_name LIKE $4 AND m.col_version %% 2 = 1 "
    "ORDER BY b.col_name COLLATE \"C\"";
