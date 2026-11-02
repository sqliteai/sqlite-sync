//
//  cloudsync_changes_sqlite.h
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#ifndef __CLOUDSYNC_CHANGES_SQLITE__
#define __CLOUDSYNC_CHANGES_SQLITE__

#include "../cloudsync.h"

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

// Per-scan state of one cloudsync_changes cursor, passed to cloudsync_col_value as a
// bound pointer (4th argument) so it never outlives or crosses the statement.
#define CLOUDSYNC_CHANGES_SCAN_PTR      "cloudsync_changes_scan"
typedef struct cloudsync_changes_scan cloudsync_changes_scan;

int cloudsync_vtab_register_changes (sqlite3 *db, cloudsync_context *xdata);

bool changes_scan_pk_hidden (cloudsync_changes_scan *scan, sqlite3 *db, cloudsync_table_context *table, const char *table_name, sqlite3_stmt *lookup, const void *pk, int pklen);
void changes_scan_note_hidden (cloudsync_changes_scan *scan, cloudsync_table_context *table);
bool changes_scan_take_value (cloudsync_changes_scan *scan, cloudsync_table_context *table, const char *col_name, const void *pk, int pklen, sqlite3_context *context);
void changes_scan_keep_value (cloudsync_changes_scan *scan, cloudsync_table_context *table, const char *col_name, const void *pk, int pklen, sqlite3_value *value);

#endif
