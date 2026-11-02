//
//  meta_tables.h
//  Which tables of a SQLite database count as CloudSync metadata tables
//

#ifndef __CLOUDSYNC_SQLITE_META_TABLES__
#define __CLOUDSYNC_SQLITE_META_TABLES__

// Condition fragment for a query over sqlite_master that already selects type = 'table'.
// A table only counts as the metadata table of X when it is named X_cloudsync, X is registered
// in cloudsync_table_settings (case-insensitively, like SQLite names) and it has the columns
// cloudsync_init creates. A lookalike (junk_cloudsync) would otherwise make
// cloudsync_db_version() and cloudsync_changes fail or report a poisoned version. The base
// table may be gone: its metadata still counts toward db_version, so the version never goes back.
#define CLOUDSYNC_SQLITE_META_FILTER \
    "tbl_name LIKE '%\\_cloudsync' ESCAPE '\\' " \
    "AND EXISTS (SELECT 1 FROM cloudsync_table_settings AS s " \
    "WHERE s.tbl_name = substr(sqlite_master.tbl_name, 1, length(sqlite_master.tbl_name) - 10)) " \
    "AND (SELECT count(*) FROM pragma_table_info(sqlite_master.tbl_name) " \
    "WHERE name IN ('pk','col_name','col_version','db_version','seq','site_id')) = 6"

// The same, restricted to metadata tables whose base table still exists.
#define CLOUDSYNC_SQLITE_META_WITH_BASE_FILTER \
    CLOUDSYNC_SQLITE_META_FILTER " " \
    "AND EXISTS (SELECT 1 FROM sqlite_master AS base WHERE base.type = 'table' " \
    "AND base.name = substr(sqlite_master.tbl_name, 1, length(sqlite_master.tbl_name) - 10) COLLATE NOCASE)"

#endif
