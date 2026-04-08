//
//  sql_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 17/12/25.
//

#include "../sql.h"

// MARK: Settings

const char * const SQL_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_settings WHERE key=?1;";

const char * const SQL_SETTINGS_SET_KEY_VALUE_REPLACE =
    "REPLACE INTO cloudsync_settings (key, value) VALUES (?1, ?2);";

const char * const SQL_SETTINGS_SET_KEY_VALUE_DELETE =
    "DELETE FROM cloudsync_settings WHERE key = ?1;";

const char * const SQL_TABLE_SETTINGS_GET_VALUE =
    "SELECT value FROM cloudsync_table_settings WHERE (tbl_name=?1 AND col_name=?2 AND key=?3);";

const char * const SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE =
    "DELETE FROM cloudsync_table_settings WHERE tbl_name=?1;";

const char * const SQL_TABLE_SETTINGS_REPLACE =
    "REPLACE INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES (?1, ?2, ?3, ?4);";

const char * const SQL_TABLE_SETTINGS_DELETE_ONE =
    "DELETE FROM cloudsync_table_settings WHERE (tbl_name=?1 AND col_name=?2 AND key=?3);";

const char * const SQL_TABLE_SETTINGS_COUNT_TABLES =
    "SELECT count(*) FROM cloudsync_table_settings WHERE key='algo';";

const char * const SQL_SETTINGS_LOAD_GLOBAL =
    "SELECT key, value FROM cloudsync_settings;";

const char * const SQL_SETTINGS_LOAD_TABLE =
    "SELECT lower(tbl_name), lower(col_name), key, value FROM cloudsync_table_settings ORDER BY tbl_name, col_name;";

const char * const SQL_CREATE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_settings (key TEXT PRIMARY KEY NOT NULL COLLATE NOCASE, value TEXT);";

// format strings (sqlite3_snprintf) are also static SQL templates
const char * const SQL_INSERT_SETTINGS_STR_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%q', '%q');";

const char * const SQL_INSERT_SETTINGS_INT_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', %lld);";

const char * const SQL_CREATE_SITE_ID_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_site_id (site_id BLOB UNIQUE NOT NULL);";

const char * const SQL_INSERT_SITE_ID_ROWID =
    "INSERT INTO cloudsync_site_id (rowid, site_id) VALUES (?, ?);";

const char * const SQL_CREATE_TABLE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_table_settings (tbl_name TEXT NOT NULL COLLATE NOCASE, col_name TEXT NOT NULL COLLATE NOCASE, key TEXT, value TEXT, PRIMARY KEY(tbl_name,col_name,key));";

const char * const SQL_CREATE_SCHEMA_VERSIONS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (hash INTEGER PRIMARY KEY, seq INTEGER NOT NULL)";

const char * const SQL_SETTINGS_CLEANUP_DROP_ALL =
    "DROP TABLE IF EXISTS cloudsync_settings; "
    "DROP TABLE IF EXISTS cloudsync_site_id; "
    "DROP TABLE IF EXISTS cloudsync_table_settings; "
    "DROP TABLE IF EXISTS cloudsync_schema_versions; ";

// MARK: CloudSync

const char * const SQL_DBVERSION_BUILD_QUERY =
    "WITH table_names AS ("
    "SELECT format('%w', name) as tbl_name "
    "FROM sqlite_master "
    "WHERE type='table' "
    "AND name LIKE '%_cloudsync'"
    "), "
    "query_parts AS ("
    "SELECT 'SELECT max(db_version) as version FROM \"' || tbl_name || '\"' as part FROM table_names"
    "), "
    "combined_query AS ("
    "SELECT GROUP_CONCAT(part, ' UNION ALL ') || ' UNION SELECT value as version FROM cloudsync_settings WHERE key = ''pre_alter_dbversion''' as full_query FROM query_parts"
    ") "
    "SELECT 'SELECT max(version) as version FROM (' || full_query || ');' FROM combined_query;";

const char * const SQL_SITEID_SELECT_ROWID0 =
    "SELECT site_id FROM cloudsync_site_id WHERE rowid=0;";

const char * const SQL_DATA_VERSION =
    "PRAGMA data_version;";

const char * const SQL_SCHEMA_VERSION =
    "PRAGMA schema_version;";

const char * const SQL_SITEID_GETSET_ROWID_BY_SITEID =
    "INSERT INTO cloudsync_site_id (site_id) VALUES (?) "
    "ON CONFLICT(site_id) DO UPDATE SET site_id = site_id "
    "RETURNING rowid;";

// Format
const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID =
    "WITH col_names AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"', ',') AS cols "
    "FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid"
    ") "
    "SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM \"%w\" WHERE rowid=?;'";

const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_PK =
    "WITH col_names AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"', ',') AS cols "
    "FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid"
    "), "
    "pk_where AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"', '=? AND ') || '=?' AS pk_clause "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    ") "
    "SELECT 'SELECT ' || (SELECT cols FROM col_names) || ' FROM \"%w\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_DELETE_ROW_BY_ROWID =
    "DELETE FROM \"%w\" WHERE rowid=?;";

const char * const SQL_BUILD_DELETE_ROW_BY_PK =
    "WITH pk_where AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"', '=? AND ') || '=?' AS pk_clause "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    ") "
    "SELECT 'DELETE FROM \"%w\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_INSERT_ROWID_IGNORE =
    "INSERT OR IGNORE INTO \"%w\" (rowid) VALUES (?);";

const char * const SQL_UPSERT_ROWID_AND_COL_BY_ROWID =
    "INSERT INTO \"%w\" (rowid, \"%w\") VALUES (?, ?) ON CONFLICT DO UPDATE SET \"%w\"=?;";

const char * const SQL_BUILD_INSERT_PK_IGNORE =
    "WITH pk_where AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"') AS pk_clause "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    "), "
    "pk_bind AS ("
    "SELECT group_concat('?') AS pk_binding "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    ") "
    "SELECT 'INSERT OR IGNORE INTO \"%w\" (' || (SELECT pk_clause FROM pk_where) || ') VALUES ('  || (SELECT pk_binding FROM pk_bind) || ');'";

const char * const SQL_BUILD_UPSERT_PK_AND_COL =
    "WITH pk_where AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"') AS pk_clause "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    "), "
    "pk_bind AS ("
    "SELECT group_concat('?') AS pk_binding "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    ") "
    "SELECT 'INSERT INTO \"%w\" (' || (SELECT pk_clause FROM pk_where) || ',\"%w\") VALUES ('  || (SELECT pk_binding FROM pk_bind) || ',?) ON CONFLICT DO UPDATE SET \"%w\"=?;'";

const char * const SQL_SELECT_COLS_BY_ROWID_FMT =
    "SELECT %s%w%s FROM \"%w\" WHERE rowid=?;";

const char * const SQL_BUILD_SELECT_COLS_BY_PK_FMT =
    "WITH pk_where AS ("
    "SELECT group_concat('\"' || format('%%w', name) || '\"', '=? AND ') || '=?' AS pk_clause "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk"
    ") "
    "SELECT 'SELECT %s%w%s FROM \"%w\" WHERE ' || (SELECT pk_clause FROM pk_where) || ';'";

const char * const SQL_CLOUDSYNC_ROW_EXISTS_BY_PK =
    "SELECT EXISTS(SELECT 1 FROM \"%w\" WHERE pk = ? LIMIT 1);";

const char * const SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION =
    "UPDATE \"%w\" "
    "SET col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, "
    "db_version = ?, seq = ?, site_id = 0 "
    "WHERE pk = ? AND col_name = '%s';";

const char * const SQL_CLOUDSYNC_UPSERT_COL_INIT_OR_BUMP_VERSION =
    "INSERT INTO \"%w\" (pk, col_name, col_version, db_version, seq, site_id) "
    "SELECT ?, '%s', 1, ?, ?, 0 "
    "WHERE 1 "
    "ON CONFLICT DO UPDATE SET "
    "col_version = CASE col_version %% 2 WHEN 0 THEN col_version + 1 ELSE col_version + 2 END, "
    "db_version = ?, seq = ?, site_id = 0;";

const char * const SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION =
    "INSERT INTO \"%w\" (pk, col_name, col_version, db_version, seq, site_id ) "
    "SELECT ?, ?, ?, ?, ?, 0 "
    "WHERE 1 "
    "ON CONFLICT DO UPDATE SET "
    "col_version = \"%w\".col_version + 1, db_version = ?, seq = ?, site_id = 0;";

const char * const SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL =
    "DELETE FROM \"%w\" WHERE pk=? AND col_name!='%s';";

const char * const SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL =
    "UPDATE OR REPLACE \"%w\" "
    "SET pk=?, db_version=?, col_version=1, seq=cloudsync_seq(), site_id=0 "
    "WHERE (pk=? AND col_name!='%s');";

const char * const SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS =
    "SELECT COALESCE("
    "(SELECT col_version FROM \"%w\" WHERE pk=? AND col_name='%s'), "
    "(SELECT 1 FROM \"%w\" WHERE pk=?)"
    ");";

const char * const SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID =
    "INSERT OR REPLACE INTO \"%w\" "
    "(pk, col_name, col_version, db_version, seq, site_id) "
    "VALUES (?, ?, ?, cloudsync_db_version_next(?), ?, ?) "
    "RETURNING ((db_version << 30) | seq);";

const char * const SQL_CLOUDSYNC_TOMBSTONE_PK_EXCEPT_COL =
    "UPDATE \"%w\" "
    "SET col_version = 0, db_version = cloudsync_db_version_next(?) "
    "WHERE pk=? AND col_name!='%s';";

const char * const SQL_CLOUDSYNC_SELECT_COL_VERSION_BY_PK_COL =
    "SELECT col_version FROM \"%w\" WHERE pk=? AND col_name=?;";

const char * const SQL_CLOUDSYNC_SELECT_SITE_ID_BY_PK_COL =
    "SELECT site_id FROM \"%w\" WHERE pk=? AND col_name=?;";

const char * const SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID =
    "SELECT name, cid FROM pragma_table_info('%q') WHERE pk=0 ORDER BY cid;";

const char * const SQL_DROP_CLOUDSYNC_TABLE =
    "DROP TABLE IF EXISTS \"%w\";";

const char * const SQL_DELETE_ALL_FROM_CLOUDSYNC_TABLE =
    "DELETE FROM \"%w\";";

const char * const SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL =
    "DELETE FROM \"%w\" WHERE \"col_name\" NOT IN ("
    "SELECT name FROM pragma_table_info('%q') UNION SELECT '%s'"
    ")";

const char * const SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT =
    "SELECT group_concat('\"%w\".\"' || format('%%w', name) || '\"', ',') "
    "FROM pragma_table_info('%s') WHERE pk>0 ORDER BY pk;";

const char * const SQL_CLOUDSYNC_GC_DELETE_ORPHANED_PK =
    "DELETE FROM \"%w\" "
    "WHERE (\"col_name\" != '%s' OR (\"col_name\" = '%s' AND col_version %% 2 != 0)) "
    "AND NOT EXISTS ("
    "SELECT 1 FROM \"%w\" "
    "WHERE \"%w\".pk = cloudsync_pk_encode(%s) LIMIT 1"
    ");";

const char * const SQL_PRAGMA_TABLEINFO_PK_COLLIST =
    "SELECT group_concat('\"' || format('%%w', name) || '\"', ',') "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;";

const char * const SQL_PRAGMA_TABLEINFO_PK_DECODE_SELECTLIST =
    "SELECT group_concat("
    "'cloudsync_pk_decode(pk, ' || pk || ') AS ' || '\"' || format('%%w', name) || '\"', ','"
    ") "
    "FROM pragma_table_info('%q') WHERE pk>0 ORDER BY pk;";

const char * const SQL_CLOUDSYNC_INSERT_MISSING_PKS_FROM_BASE_EXCEPT_SYNC =
    "SELECT cloudsync_insert('%q', %s) "
    "FROM (SELECT %s FROM \"%w\" EXCEPT SELECT %s FROM \"%w\");";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM \"%w\") "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM \"%w\" _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = ?"
    ");";

const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL_FILTERED =
    "WITH _cstemp1 AS (SELECT cloudsync_pk_encode(%s) AS pk FROM \"%w\" WHERE (%s)) "
    "SELECT _cstemp1.pk FROM _cstemp1 "
    "WHERE NOT EXISTS ("
    "SELECT 1 FROM \"%w\" _cstemp2 "
    "WHERE _cstemp2.pk = _cstemp1.pk AND _cstemp2.col_name = ?"
    ");";

const char * const SQL_CHANGES_INSERT_ROW =
    "INSERT INTO cloudsync_changes(tbl, pk, col_name, col_value, col_version, db_version, site_id, cl, seq) "
    "VALUES (?,?,?,?,?,?,?,?,?);";

// MARK: Blocks (block-level LWW)

const char * const SQL_BLOCKS_CREATE_TABLE =
    "CREATE TABLE IF NOT EXISTS %s ("
    "pk BLOB NOT NULL, "
    "col_name TEXT NOT NULL, "
    "col_value BLOB, "
    "PRIMARY KEY (pk, col_name)) WITHOUT ROWID";

const char * const SQL_BLOCKS_UPSERT =
    "INSERT OR REPLACE INTO %s (pk, col_name, col_value) VALUES (?1, ?2, ?3)";

const char * const SQL_BLOCKS_SELECT =
    "SELECT col_value FROM %s WHERE pk = ?1 AND col_name = ?2";

const char * const SQL_BLOCKS_DELETE =
    "DELETE FROM %s WHERE pk = ?1 AND col_name = ?2";

const char * const SQL_BLOCKS_LIST_ALIVE =
    "SELECT b.col_value FROM %s b "
    "JOIN %s m ON b.pk = m.pk AND b.col_name = m.col_name "
    "WHERE b.pk = ?1 AND b.col_name LIKE ?2 "
    "AND m.pk = ?3 AND m.col_name LIKE ?4 AND m.col_version %% 2 = 1 "
    "ORDER BY b.col_name";
