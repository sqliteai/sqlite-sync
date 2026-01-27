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
#include "database.h"

// CLOUDSYNC_DESKTOP_OS = 1 if compiling for macOS, Linux (desktop), or Windows
// Not set for iOS, Android, WebAssembly, or other platforms
#if defined(_WIN32) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    #define CLOUDSYNC_DESKTOP_OS 1
#elif defined(__APPLE__) && defined(__MACH__)
    #include <TargetConditionals.h>
    #if TARGET_OS_OSX
        #define CLOUDSYNC_DESKTOP_OS 1
    #endif
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    #define CLOUDSYNC_DESKTOP_OS 1
#endif

#define CLOUDSYNC_DEBUG_FUNCTIONS           0
#define CLOUDSYNC_DEBUG_DBFUNCTIONS         0
#define CLOUDSYNC_DEBUG_SETTINGS            0
#define CLOUDSYNC_DEBUG_SQL                 0
#define CLOUDSYNC_DEBUG_VTAB                0
#define CLOUDSYNC_DEBUG_MEMORY              0
#define CLOUDSYNC_DEBUG_STMT                0
#define CLOUDSYNC_DEBUG_MERGE               0

// Debug macros - platform-specific logging
#ifdef CLOUDSYNC_POSTGRESQL_BUILD
    // PostgreSQL build - use elog() for logging
    #include "postgresql/postgresql_log.h"
    #define DEBUG_RUNTIME(...)              do {if (data->debug) CLOUDSYNC_LOG_DEBUG(__VA_ARGS__ );} while (0)
    #define DEBUG_PRINTLN(...)              CLOUDSYNC_LOG_DEBUG(__VA_ARGS__)
    #define DEBUG_ALWAYS(...)               CLOUDSYNC_LOG_INFO(__VA_ARGS__)
    #define DEBUG_PRINT(...)                CLOUDSYNC_LOG_DEBUG(__VA_ARGS__)
#else
    // SQLite and other platforms use printf()
    #define DEBUG_RUNTIME(...)              do {if (data->debug) printf(__VA_ARGS__ );} while (0)
    #define DEBUG_PRINTLN(...)              do {printf(__VA_ARGS__ );printf("\n");} while (0)
    #define DEBUG_ALWAYS(...)               do {printf(__VA_ARGS__ );printf("\n");} while (0)
    #define DEBUG_PRINT(...)                do {printf(__VA_ARGS__ );} while (0)
#endif

#if CLOUDSYNC_DEBUG_FUNCTIONS
#define DEBUG_FUNCTION(...)                 DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_FUNCTION(...)
#endif

#if CLOUDSYNC_DEBUG_DBFUNCTIONS
#define DEBUG_DBFUNCTION(...)               DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_DBFUNCTION(...)
#endif

#if CLOUDSYNC_DEBUG_SETTINGS
#define DEBUG_SETTINGS(...)                 DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_SETTINGS(...)
#endif

#if CLOUDSYNC_DEBUG_SQL
#define DEBUG_SQL(...)                      DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_SQL(...)
#endif

#if CLOUDSYNC_DEBUG_VTAB
#define DEBUG_VTAB(...)                     DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_VTAB(...)
#endif

#if CLOUDSYNC_DEBUG_STMT
#define DEBUG_STMT(...)                     DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_STMT(...)
#endif

#if CLOUDSYNC_DEBUG_MERGE
#define DEBUG_MERGE(...)                    DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_MERGE(...)
#endif

#if CLOUDSYNC_DEBUG_MEMORY
#define cloudsync_memory_init(_once)                  memdebug_init(_once)
#define cloudsync_memory_finalize                     memdebug_finalize
#define cloudsync_memory_alloc                        memdebug_alloc
#define cloudsync_memory_zeroalloc                    memdebug_zeroalloc
#define cloudsync_memory_free                         memdebug_free
#define cloudsync_memory_realloc                      memdebug_realloc
#define cloudsync_memory_size                         memdebug_msize
#define cloudsync_memory_mprintf                      memdebug_mprintf

void memdebug_init (int once);
void memdebug_finalize (void);
void *memdebug_alloc (uint64_t size);
void *memdebug_zeroalloc (uint64_t size);
void *memdebug_realloc (void *ptr, uint64_t new_size);
char *memdebug_vmprintf (const char *format, va_list list);
char *memdebug_mprintf(const char *format, ...);
void memdebug_free (void *ptr);
uint64_t memdebug_msize (void *ptr);
#else
#define cloudsync_memory_init(_once)
#define cloudsync_memory_finalize()
#define cloudsync_memory_alloc                        dbmem_alloc
#define cloudsync_memory_zeroalloc                    dbmem_zeroalloc
#define cloudsync_memory_free                         dbmem_free
#define cloudsync_memory_realloc                      dbmem_realloc
#define cloudsync_memory_size                         dbmem_size
#define cloudsync_memory_mprintf                      dbmem_mprintf
#endif

#define UUID_STR_MAXLEN                     37
#define UUID_LEN                            16

int cloudsync_uuid_v7 (uint8_t value[UUID_LEN]);
int cloudsync_uuid_v7_compare (uint8_t value1[UUID_LEN], uint8_t value2[UUID_LEN]);
char *cloudsync_uuid_v7_string (char value[UUID_STR_MAXLEN], bool dash_format);
char *cloudsync_uuid_v7_stringify (uint8_t uuid[UUID_LEN], char value[UUID_STR_MAXLEN], bool dash_format);
uint64_t fnv1a_hash(const char *data, size_t len);

char *cloudsync_string_replace_prefix(const char *input, char *prefix, char *replacement);
char *cloudsync_string_dup (const char *str);
char *cloudsync_string_dup_lowercase (const char *str);
char *cloudsync_string_ndup (const char *str, size_t len);
char *cloudsync_string_ndup_lowercase (const char *str, size_t len);

int cloudsync_blob_compare(const char *blob1, size_t size1, const char *blob2, size_t size2);

void cloudsync_rowid_decode (int64_t rowid, int64_t *db_version, int64_t *seq);

// available only on Desktop OS (no WASM, no mobile)
#ifdef CLOUDSYNC_DESKTOP_OS
bool cloudsync_file_delete (const char *path);
char *cloudsync_file_read (const char *path, int64_t *len);
bool cloudsync_file_write (const char *path, const char *buffer, size_t len);
#endif

#endif
