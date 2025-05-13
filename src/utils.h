//
//  utils.h
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#ifndef __CLOUDSYNC_UTILS__
#define __CLOUDSYNC_UTILS__

#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <string.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#define CLOUDSYNC_DEBUG_FUNCTIONS           0
#define CLOUDSYNC_DEBUG_DBFUNCTIONS         0
#define CLOUDSYNC_DEBUG_SETTINGS            0
#define CLOUDSYNC_DEBUG_SQL                 0
#define CLOUDSYNC_DEBUG_VTAB                0
#define CLOUDSYNC_DEBUG_MEMORY              0
#define CLOUDSYNC_DEBUG_STMT                0
#define CLOUDSYNC_DEBUG_MERGE               0

#define DEBUG_RUNTIME(...)                  do {if (data->debug) printf(__VA_ARGS__ );} while (0)
#define DEBUG_PRINTLN(...)                  do {printf(__VA_ARGS__ );printf("\n");} while (0)
#define DEBUG_ALWAYS(...)                   do {printf(__VA_ARGS__ );printf("\n");} while (0)
#define DEBUG_PRINT(...)                    do {printf(__VA_ARGS__ );} while (0)

#if CLOUDSYNC_DEBUG_FUNCTIONS
#define DEBUG_FUNCTION(...)                 do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_FUNCTION(...)
#endif

#if CLOUDSYNC_DEBUG_DBFUNCTION
#define DEBUG_DBFUNCTION(...)               do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_DBFUNCTION(...)
#endif

#if CLOUDSYNC_DEBUG_SETTINGS
#define DEBUG_SETTINGS(...)                 do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_SETTINGS(...)
#endif

#if CLOUDSYNC_DEBUG_SQL
#define DEBUG_SQL(...)                      do {printf(__VA_ARGS__ );printf("\n\n");} while (0)
#else
#define DEBUG_SQL(...)
#endif

#if CLOUDSYNC_DEBUG_VTAB
#define DEBUG_VTAB(...)                     do {printf(__VA_ARGS__ );printf("\n\n");} while (0)
#else
#define DEBUG_VTAB(...)
#endif

#if CLOUDSYNC_DEBUG_STMT
#define DEBUG_STMT(...)                     do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_STMT(...)
#endif

#if CLOUDSYNC_DEBUG_MERGE
#define DEBUG_MERGE(...)                     do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_MERGE(...)
#endif

#if CLOUDSYNC_DEBUG_MEMORY
#define cloudsync_memory_init(_once)                  memdebug_init(_once)
#define cloudsync_memory_finalize                     memdebug_finalize
#define cloudsync_memory_alloc                        memdebug_alloc
#define cloudsync_memory_free                         memdebug_free
#define cloudsync_memory_realloc                      memdebug_realloc
#define cloudsync_memory_size                         memdebug_msize
#define cloudsync_memory_vmprintf                     memdebug_vmprintf
#define cloudsync_memory_mprintf                      memdebug_mprintf

void memdebug_init (int once);
void memdebug_finalize (void);
void *memdebug_alloc (sqlite3_uint64 size);
void *memdebug_realloc (void *ptr, sqlite3_uint64 new_size);
char *memdebug_vmprintf (const char *format, va_list list);
char *memdebug_mprintf(const char *format, ...);
void memdebug_free (void *ptr);
sqlite3_uint64 memdebug_msize (void *ptr);
#else
#define cloudsync_memory_init(_once)
#define cloudsync_memory_finalize()
#define cloudsync_memory_alloc                        sqlite3_malloc64
#define cloudsync_memory_free                         sqlite3_free
#define cloudsync_memory_realloc                      sqlite3_realloc64
#define cloudsync_memory_size                         sqlite3_msize
#define cloudsync_memory_vmprintf                     sqlite3_vmprintf
#define cloudsync_memory_mprintf                      sqlite3_mprintf
#endif

#define UUID_STR_MAXLEN                     37
#define UUID_LEN                            16

// The type of CRDT chosen for a table controls what rows are included or excluded when merging tables together from different databases
typedef enum {
    table_algo_none = 0,
    table_algo_crdt_cls = 100,   // CausalLengthSet
    table_algo_crdt_gos,         // GrowOnlySet
    table_algo_crdt_dws,         // DeleteWinsSet
    table_algo_crdt_aws          // AddWinsSet
} table_algo;

table_algo crdt_algo_from_name (const char *name);
const char *crdt_algo_name (table_algo algo);

int cloudsync_uuid_v7 (uint8_t value[UUID_LEN]);
int cloudsync_uuid_v7_compare (uint8_t value1[UUID_LEN], uint8_t value2[UUID_LEN]);
char *cloudsync_uuid_v7_string (char value[UUID_STR_MAXLEN], bool dash_format);
char *cloudsync_uuid_v7_stringify (uint8_t uuid[UUID_LEN], char value[UUID_STR_MAXLEN], bool dash_format);
uint64_t fnv1a_hash(const char *data, size_t len);

void *cloudsync_memory_zeroalloc (uint64_t size);
char *cloudsync_string_dup (const char *str, bool lowercase);
void cloudsync_rowid_decode (sqlite3_int64 rowid, sqlite3_int64 *db_version, sqlite3_int64 *seq);

#endif
