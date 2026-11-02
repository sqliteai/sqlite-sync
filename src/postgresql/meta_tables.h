//
//  meta_tables.h
//  Which relations count as CloudSync metadata tables
//

#ifndef __CLOUDSYNC_PG_META_TABLES__
#define __CLOUDSYNC_PG_META_TABLES__

// WHERE-clause fragment over pg_class c JOIN pg_namespace n. A table is only the CloudSync
// metadata table of X when X is registered in cloudsync_table_settings and the table has the
// shape cloudsync_init creates. A lookalike (junk_cloudsync) would otherwise poison
// cloudsync_db_version() and cloudsync_changes, or make them fail. The base table may be gone:
// its metadata still counts toward db_version, so the version never goes back.
// Requires cloudsync_table_settings to exist.
#define CLOUDSYNC_PG_META_TABLE_FILTER \
    "c.relkind = 'r' " \
    "AND n.nspname NOT IN ('pg_catalog','information_schema') " \
    "AND c.relname LIKE '%\\_cloudsync' ESCAPE '\\' " \
    "AND EXISTS (SELECT 1 FROM cloudsync_table_settings s WHERE s.tbl_name = left(c.relname, -10)) " \
    "AND (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 " \
    "AND NOT a.attisdropped AND a.attname IN ('pk','col_name','col_version','db_version','seq','site_id')) = 6"

// The same, restricted to metadata tables whose base table still exists beside them.
#define CLOUDSYNC_PG_META_TABLE_WITH_BASE_FILTER \
    CLOUDSYNC_PG_META_TABLE_FILTER " " \
    "AND EXISTS (SELECT 1 FROM pg_class b WHERE b.relnamespace = c.relnamespace " \
    "AND b.relname = left(c.relname, -10) AND b.relkind IN ('r','p'))"

#endif
