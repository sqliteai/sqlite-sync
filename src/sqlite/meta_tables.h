//
//  meta_tables.h
//  Which tables of a SQLite database count as CloudSync metadata tables
//

#ifndef __CLOUDSYNC_SQLITE_META_TABLES__
#define __CLOUDSYNC_SQLITE_META_TABLES__

// Condition fragment for a query over sqlite_master that already selects type = 'table'.
// A table only counts as the metadata table of X when it is named X_cloudsync, X is a table
// of the same database and it has the columns cloudsync_init creates. A lookalike
// (junk_cloudsync) would otherwise make cloudsync_db_version() and cloudsync_changes fail or
// report a poisoned version.
#define CLOUDSYNC_SQLITE_META_FILTER \
    "tbl_name LIKE '%\\_cloudsync' ESCAPE '\\' " \
    "AND EXISTS (SELECT 1 FROM sqlite_master AS base WHERE base.type = 'table' " \
    "AND base.name = substr(sqlite_master.tbl_name, 1, length(sqlite_master.tbl_name) - 10)) " \
    "AND (SELECT count(*) FROM pragma_table_info(sqlite_master.tbl_name) " \
    "WHERE name IN ('pk','col_name','col_version','db_version','seq','site_id')) = 6"

#endif
