//
//  meta_tables.h
//  Which relations count as CloudSync metadata tables
//

#ifndef __CLOUDSYNC_PG_META_TABLES__
#define __CLOUDSYNC_PG_META_TABLES__

// WHERE-clause fragment over pg_class c JOIN pg_namespace n. A table is only a CloudSync
// metadata table when it has the shape cloudsync_init creates, sits beside its base table
// under the same owner, and the caller can read it. A same-named table made by someone else
// (junk_cloudsync) would otherwise poison cloudsync_db_version() and cloudsync_changes, or
// make them fail on a permission error.
#define CLOUDSYNC_PG_META_TABLE_FILTER \
    "c.relkind = 'r' " \
    "AND n.nspname NOT IN ('pg_catalog','information_schema') " \
    "AND c.relname LIKE '%\\_cloudsync' ESCAPE '\\' " \
    "AND has_table_privilege(c.oid, 'SELECT') " \
    "AND EXISTS (SELECT 1 FROM pg_class b WHERE b.relnamespace = c.relnamespace " \
    "AND b.relname = left(c.relname, -10) AND b.relkind IN ('r','p') AND b.relowner = c.relowner) " \
    "AND (SELECT count(*) FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 " \
    "AND NOT a.attisdropped AND a.attname IN ('pk','col_name','col_version','db_version','seq','site_id')) = 6"

#endif
