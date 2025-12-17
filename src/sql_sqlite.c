//
//  sql_sqlite.c
//  cloudsync
//
//  Created by Marco Bambini on 17/12/25.
//

#include "sql.h"

// MARK: - Settings -

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
    "SELECT lower(tbl_name), lower(col_name), key, value FROM cloudsync_table_settings ORDER BY tbl_name;";

const char * const SQL_CREATE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_settings (key TEXT PRIMARY KEY NOT NULL COLLATE NOCASE, value TEXT);";

// format strings (snprintf) are also static SQL templates
const char * const SQL_INSERT_SETTINGS_STR_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', '%s');";

const char * const SQL_INSERT_SETTINGS_INT_FORMAT =
    "INSERT INTO cloudsync_settings (key, value) VALUES ('%s', %lld);";

const char * const SQL_CREATE_SITE_ID_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_site_id (site_id BLOB UNIQUE NOT NULL);";

const char * const SQL_INSERT_SITE_ID_ROWID =
    "INSERT INTO cloudsync_site_id (rowid, site_id) VALUES (?, ?);";

const char * const SQL_CREATE_TABLE_SETTINGS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_table_settings (tbl_name TEXT NOT NULL COLLATE NOCASE, col_name TEXT NOT NULL COLLATE NOCASE, key TEXT, value TEXT, PRIMARY KEY(tbl_name,key));";

const char * const SQL_CREATE_SCHEMA_VERSIONS_TABLE =
    "CREATE TABLE IF NOT EXISTS cloudsync_schema_versions (hash INTEGER PRIMARY KEY, seq INTEGER NOT NULL)";

const char * const SQL_SETTINGS_CLEANUP_DROP_ALL =
    "DROP TABLE IF EXISTS cloudsync_settings; "
    "DROP TABLE IF EXISTS cloudsync_site_id; "
    "DROP TABLE IF EXISTS cloudsync_table_settings; "
    "DROP TABLE IF EXISTS cloudsync_schema_versions; ";

