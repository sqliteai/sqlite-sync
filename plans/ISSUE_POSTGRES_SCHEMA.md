Issue summary

cloudsync_init('users') fails in Supabase postgres with:
"column reference \"id\" is ambiguous".
Both public.users and auth.users exist. Several PostgreSQL SQL templates use only table_name (no schema), so information_schema lookups and dynamic SQL see multiple tables and generate ambiguous column references.

Proposed fixes (options)

1) Minimal fix (patch specific templates)
- Add table_schema = current_schema() to information_schema queries.
- Keep relying on search_path.
- Resolves Supabase default postgres collisions without changing the API.

2) Robust fix (explicit schema support)
- Allow schema-qualified inputs, e.g. cloudsync_init('public.users').
- Parse schema/table and propagate through query builders.
- Always generate fully-qualified table names ("schema"."table").
- Apply schema-aware filters in information_schema queries.
- Removes ambiguity regardless of search_path or duplicate table names across schemas.
- Note: payload compatibility requires cloudsync_changes.tbl to remain unqualified; PG apply should resolve schema via cloudsync_table_settings (not search_path) when applying payloads.

Bugged query templates

Already fixed:
- SQL_PRAGMA_TABLEINFO_PK_COLLIST
- SQL_PRAGMA_TABLEINFO_PK_DECODE_SELECTLIST

Still vulnerable (missing schema filter):
- SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID
- SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID
- SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL
- SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT

Robust fix implementation plan

Goals
- Support cloudsync_init('users') and cloudsync_init('public.users')
- Default schema to current_schema() when not provided
- Persist schema so future connections are independent of search_path
- Generate fully qualified table names in all PostgreSQL SQL builders

1) Parse schema/table at init
- In cloudsync_init_table() (cloudsync.c), parse the input table_name:
  - If it contains a dot, split schema/table
  - Else schema = current_schema() (query once)
- Normalize case to match existing behavior

2) Persist schema in settings
- Store schema in cloudsync_table_settings using key='schema'
- Keep tbl_name as unqualified table name
- On first run, if schema is not stored, write it

3) Store schema in context
- Add char *schema to cloudsync_table_context
- Populate on table creation and when reloading from settings
- Use schema when building SQL

4) Restore schema on new connections
- During context rebuild, read schema from cloudsync_table_settings
- If missing, fallback to current_schema(), optionally persist it

5) Qualify SQL everywhere (Postgres)
- Use "schema"."table" in generated SQL
- Add table_schema filters to information_schema queries:
  - SQL_BUILD_SELECT_NONPK_COLS_BY_ROWID
  - SQL_PRAGMA_TABLEINFO_LIST_NONPK_NAME_CID
  - SQL_CLOUDSYNC_DELETE_COLS_NOT_IN_SCHEMA_OR_PKCOL
  - SQL_PRAGMA_TABLEINFO_PK_QUALIFIED_COLLIST_FMT
  - Any other information_schema templates using only table_name

6) Compatibility
- Existing DBs without schema setting continue to work via current_schema()
- No API changes required for unqualified names
