//
//  sql.h
//  cloudsync
//
//  Created by Marco Bambini on 17/12/25.
//

#ifndef __CLOUDSYNC_SQL__
#define __CLOUDSYNC_SQL__

// SETTINGS
extern const char * const SQL_SETTINGS_GET_VALUE;
extern const char * const SQL_SETTINGS_SET_KEY_VALUE_REPLACE;
extern const char * const SQL_SETTINGS_SET_KEY_VALUE_DELETE;
extern const char * const SQL_TABLE_SETTINGS_GET_VALUE;
extern const char * const SQL_TABLE_SETTINGS_DELETE_ALL_FOR_TABLE;
extern const char * const SQL_TABLE_SETTINGS_REPLACE;
extern const char * const SQL_TABLE_SETTINGS_DELETE_ONE;
extern const char * const SQL_TABLE_SETTINGS_COUNT_TABLES;
extern const char * const SQL_SETTINGS_LOAD_GLOBAL;
extern const char * const SQL_SETTINGS_LOAD_TABLE;
extern const char * const SQL_CREATE_SETTINGS_TABLE;
extern const char * const SQL_INSERT_SETTINGS_STR_FORMAT;
extern const char * const SQL_INSERT_SETTINGS_INT_FORMAT;
extern const char * const SQL_CREATE_SITE_ID_TABLE;
extern const char * const SQL_INSERT_SITE_ID_ROWID;
extern const char * const SQL_CREATE_TABLE_SETTINGS_TABLE;
extern const char * const SQL_CREATE_SCHEMA_VERSIONS_TABLE;
extern const char * const SQL_SETTINGS_CLEANUP_DROP_ALL;

// CLOUDSYNC
extern const char * const SQL_DBVERSION_BUILD_QUERY;
extern const char * const SQL_SITEID_SELECT_ROWID0;
extern const char * const SQL_DATA_VERSION;
extern const char * const SQL_SCHEMA_VERSION;
extern const char * const SQL_SITEID_GETSET_ROWID_BY_SITEID;
extern const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID;
extern const char * const SQL_BUILD_SELECT_NONPK_COLS_BY_PK;
extern const char * const SQL_DELETE_ROW_BY_ROWID;
extern const char * const SQL_BUILD_DELETE_ROW_BY_PK;
extern const char * const SQL_INSERT_ROWID_IGNORE;
extern const char * const SQL_UPSERT_ROWID_AND_COL_BY_ROWID;
extern const char * const SQL_BUILD_INSERT_PK_IGNORE;
extern const char * const SQL_BUILD_UPSERT_PK_AND_COL;
extern const char * const SQL_SELECT_COLS_BY_ROWID_FMT;
extern const char * const SQL_BUILD_SELECT_COLS_BY_PK_FMT;
extern const char * const SQL_CLOUDSYNC_ROW_EXISTS_BY_PK;
extern const char * const SQL_CLOUDSYNC_UPDATE_COL_BUMP_VERSION;
extern const char * const SQL_CLOUDSYNC_UPSERT_COL_INIT_OR_BUMP_VERSION;
extern const char * const SQL_CLOUDSYNC_UPSERT_RAW_COLVERSION;
extern const char * const SQL_CLOUDSYNC_DELETE_PK_EXCEPT_COL;
extern const char * const SQL_CLOUDSYNC_REKEY_PK_AND_RESET_VERSION_EXCEPT_COL;
extern const char * const SQL_CLOUDSYNC_GET_COL_VERSION_OR_ROW_EXISTS;
extern const char * const SQL_CLOUDSYNC_INSERT_RETURN_CHANGE_ID;
extern const char * const SQL_CLOUDSYNC_TOMBSTONE_PK_EXCEPT_COL;
extern const char * const SQL_CLOUDSYNC_SELECT_COL_VERSION_BY_PK_COL;
extern const char * const SQL_CLOUDSYNC_SELECT_SITE_ID_BY_PK_COL;
extern const char * const SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID;
extern const char * const SQL_DROP_CLOUDSYNC_TABLE;
extern const char * const SQL_DELETE_ALL_FROM_CLOUDSYNC_TABLE;
extern const char * const SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL;
extern const char * const SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT;
extern const char * const SQL_CLOUDSYNC_GC_DELETE_ORPHANED_PK;
extern const char * const SQL_PRAGMA_TABLEINFO_PK_COLLIST;
extern const char * const SQL_PRAGMA_TABLEINFO_PK_DECODE_SELECTLIST;
extern const char * const SQL_CLOUDSYNC_INSERT_MISSING_PKS_FROM_BASE_EXCEPT_SYNC;
extern const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL;
extern const char * const SQL_CLOUDSYNC_SELECT_PKS_NOT_IN_SYNC_FOR_COL_FILTERED;
extern const char * const SQL_CHANGES_INSERT_ROW;

// BLOCKS (block-level LWW)
extern const char * const SQL_BLOCKS_CREATE_TABLE;
extern const char * const SQL_BLOCKS_UPSERT;
extern const char * const SQL_BLOCKS_SELECT;
extern const char * const SQL_BLOCKS_DELETE;
extern const char * const SQL_BLOCKS_LIST_ALIVE;

#endif
