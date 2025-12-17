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

#endif
