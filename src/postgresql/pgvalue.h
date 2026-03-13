//  pgvalue.h
//  PostgreSQL-specific dbvalue_t wrapper

#ifndef CLOUDSYNC_PGVALUE_H
#define CLOUDSYNC_PGVALUE_H

// Define POSIX feature test macros before any includes
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "postgres.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/array.h"
#include "../database.h"

// dbvalue_t representation for PostgreSQL. We capture Datum + type metadata so
// value helpers can resolve type/length/ownership without relying on fcinfo lifetime.
typedef struct pgvalue_t {
    Datum   datum;
    Oid     typeid;
    int32   typmod;
    Oid     collation;
    bool    isnull;
    bool    detoasted;
    void    *owned_detoast;
    char    *cstring;
    bool    owns_cstring;
} pgvalue_t;

pgvalue_t *pgvalue_create(Datum datum, Oid typeid, int32 typmod, Oid collation, bool isnull);
void pgvalue_free (pgvalue_t *v);
void pgvalue_ensure_detoast(pgvalue_t *v);
bool pgvalue_is_text_type(Oid typeid);
int pgvalue_dbtype(pgvalue_t *v);
pgvalue_t **pgvalues_from_array(ArrayType *array, int *out_count);
pgvalue_t **pgvalues_from_args(FunctionCallInfo fcinfo, int start_arg, int *out_count);
void pgvalues_normalize_to_text(pgvalue_t **values, int count);

#endif // CLOUDSYNC_PGVALUE_H
