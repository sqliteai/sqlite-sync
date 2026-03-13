//
//  pgvalue.c
//  PostgreSQL-specific dbvalue_t helpers
//

#include "pgvalue.h"

#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "../utils.h"

bool pgvalue_is_text_type(Oid typeid) {
    switch (typeid) {
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
        case NAMEOID:
        case JSONOID:
        case JSONBOID:
        case XMLOID:
            return true;
        default:
            return false;
    }
}

static bool pgvalue_is_varlena(Oid typeid) {
    return (typeid == BYTEAOID) || pgvalue_is_text_type(typeid);
}

pgvalue_t *pgvalue_create(Datum datum, Oid typeid, int32 typmod, Oid collation, bool isnull) {
    pgvalue_t *v = cloudsync_memory_zeroalloc(sizeof(pgvalue_t));
    if (!v) return NULL;
    
    v->datum = datum;
    v->typeid = typeid;
    v->typmod = typmod;
    v->collation = collation;
    v->isnull = isnull;
    return v;
}

void pgvalue_free (pgvalue_t *v) {
    if (!v) return;

    if (v->owned_detoast) {
        pfree(v->owned_detoast);
    }
    if (v->owns_cstring && v->cstring) {
        pfree(v->cstring);
    }
    cloudsync_memory_free(v);
}

void pgvalue_ensure_detoast(pgvalue_t *v) {
    if (!v || v->detoasted) return;
    if (!pgvalue_is_varlena(v->typeid) || v->isnull) return;

    v->owned_detoast = (void *)PG_DETOAST_DATUM_COPY(v->datum);
    v->datum = PointerGetDatum(v->owned_detoast);
    v->detoasted = true;
}

int pgvalue_dbtype(pgvalue_t *v) {
    if (!v || v->isnull) return DBTYPE_NULL;
    switch (v->typeid) {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case BOOLOID:
        case CHAROID:
        case OIDOID:
            return DBTYPE_INTEGER;
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            return DBTYPE_FLOAT;
        case BYTEAOID:
            return DBTYPE_BLOB;
        default:
            if (pgvalue_is_text_type(v->typeid)) {
                return DBTYPE_TEXT;
            }
            return DBTYPE_TEXT;
    }
}

static bool pgvalue_vec_push(pgvalue_t ***arr, int *count, int *cap, pgvalue_t *val) {
    if (*cap == 0) {
        *cap = 8;
        *arr = (pgvalue_t **)cloudsync_memory_zeroalloc(sizeof(pgvalue_t *) * (*cap));
        if (*arr == NULL) return false;
    } else if (*count >= *cap) {
        *cap *= 2;
        *arr = (pgvalue_t **)cloudsync_memory_realloc(*arr, sizeof(pgvalue_t *) * (*cap));
        if (*arr == NULL) return false;
    }
    (*arr)[(*count)++] = val;
    return true;
}

pgvalue_t **pgvalues_from_array(ArrayType *array, int *out_count) {
    if (out_count) *out_count = 0;
    if (!array) return NULL;

    Oid elem_type = ARR_ELEMTYPE(array);
    int16 elmlen;
    bool elmbyval;
    char elmalign;
    get_typlenbyvalalign(elem_type, &elmlen, &elmbyval, &elmalign);

    Datum *elems = NULL;
    bool *nulls = NULL;
    int nelems = 0;

    deconstruct_array(array, elem_type, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);

    pgvalue_t **values = NULL;
    int count = 0;
    int cap = 0;

    for (int i = 0; i < nelems; i++) {
        pgvalue_t *v = pgvalue_create(elems[i], elem_type, -1, InvalidOid, nulls ? nulls[i] : false);
        pgvalue_vec_push(&values, &count, &cap, v);
    }

    if (elems) pfree(elems);
    if (nulls) pfree(nulls);

    if (out_count) *out_count = count;
    return values;
}

pgvalue_t **pgvalues_from_args(FunctionCallInfo fcinfo, int start_arg, int *out_count) {
    if (out_count) *out_count = 0;
    if (!fcinfo) return NULL;

    pgvalue_t **values = NULL;
    int count = 0;
    int cap = 0;

    for (int i = start_arg; i < PG_NARGS(); i++) {
        Oid argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
        bool isnull = PG_ARGISNULL(i);

        // If the argument is an array (used for VARIADIC pk functions), expand it.
        Oid elemtype = InvalidOid;
        if (OidIsValid(argtype)) {
            elemtype = get_element_type(argtype);
        }

        if (OidIsValid(elemtype) && !isnull) {
            ArrayType *array = PG_GETARG_ARRAYTYPE_P(i);
            int subcount = 0;
            pgvalue_t **subvals = pgvalues_from_array(array, &subcount);
            for (int j = 0; j < subcount; j++) {
                pgvalue_vec_push(&values, &count, &cap, subvals[j]);
            }
            if (subvals) cloudsync_memory_free(subvals);
            continue;
        }

        Datum datum = isnull ? (Datum)0 : PG_GETARG_DATUM(i);
        pgvalue_t *v = pgvalue_create(datum, argtype, -1, fcinfo->fncollation, isnull);
        pgvalue_vec_push(&values, &count, &cap, v);
    }

    if (out_count) *out_count = count;
    return values;
}

void pgvalues_normalize_to_text(pgvalue_t **values, int count) {
    // Convert all non-text pgvalues to text representation.
    // This ensures PK encoding is consistent regardless of whether the caller
    // passes native types (e.g., integer 1) or text representations (e.g., '1').
    // The UPDATE trigger casts all values to ::text, so INSERT trigger and
    // SQL functions must do the same for PK encoding consistency.
    if (!values) return;

    for (int i = 0; i < count; i++) {
        pgvalue_t *v = values[i];
        if (!v || v->isnull) continue;
        if (pgvalue_is_text_type(v->typeid)) continue;

        // Convert to text using the type's output function
        const char *cstr = database_value_text((dbvalue_t *)v);
        if (!cstr) continue;

        // Create a new text datum
        text *t = cstring_to_text(cstr);
        pgvalue_t *new_v = pgvalue_create(PointerGetDatum(t), TEXTOID, -1, v->collation, false);
        if (new_v) {
            pgvalue_free(v);
            values[i] = new_v;
        }
    }
}
