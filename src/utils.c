//
//  utils.c
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#include "utils.h"
#include <ctype.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>
#include <ntstatus.h> //for STATUS_SUCCESS
#else
#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/random.h>
#endif
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

#define FNV_OFFSET_BASIS    0xcbf29ce484222325ULL
#define FNV_PRIME           0x100000001b3ULL

// MARK: UUIDv7 -

/*
    UUIDv7 is a 128-bit unique identifier like it's older siblings, such as the widely used UUIDv4.
    But unlike v4, UUIDv7 is time-sortable with 1 ms precision.
    By combining the timestamp and the random parts, UUIDv7 becomes an excellent choice for record identifiers in databases, including distributed ones.
 
    UUIDv7 offers several advantages.
    It includes a 48-bit Unix timestamp with millisecond accuracy and will overflow far in the future (10899 AD).
    It also include 74 random bits which means billions can be created every second without collisions.
    Because of its structure UUIDv7s are globally sortable and can be created in parallel in a distributed system.
 
    https://antonz.org/uuidv7/#c
    https://www.rfc-editor.org/rfc/rfc9562.html#name-uuid-version-7
 */

int cloudsync_uuid_v7 (uint8_t value[UUID_LEN]) {
    // fill the buffer with high-quality random data
    #ifdef _WIN32
    if (BCryptGenRandom(NULL, (BYTE*)value, UUID_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS) return -1;
    #elif defined(__APPLE__)
    // Use arc4random_buf for macOS/iOS
    arc4random_buf(value, UUID_LEN);
    #else
    if (getentropy(value, UUID_LEN) != 0) return -1;
    #endif
    
    // get current timestamp in ms
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) return -1;
    
    // add timestamp part to UUID
    uint64_t timestamp = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    value[0] = (timestamp >> 40) & 0xFF;
    value[1] = (timestamp >> 32) & 0xFF;
    value[2] = (timestamp >> 24) & 0xFF;
    value[3] = (timestamp >> 16) & 0xFF;
    value[4] = (timestamp >> 8) & 0xFF;
    value[5] = timestamp & 0xFF;
    
    // version and variant
    value[6] = (value[6] & 0x0F) | 0x70; // UUID version 7
    value[8] = (value[8] & 0x3F) | 0x80; // RFC 4122 variant
    
    return 0;
}

char *cloudsync_uuid_v7_stringify (uint8_t uuid[UUID_LEN], char value[UUID_STR_MAXLEN], bool dash_format) {
    if (dash_format) {
        snprintf(value, UUID_STR_MAXLEN, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
    } else {
        snprintf(value, UUID_STR_MAXLEN, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
    }
    
    return (char *)value;
}

char *cloudsync_uuid_v7_string (char value[UUID_STR_MAXLEN], bool dash_format) {
    uint8_t uuid[UUID_LEN];
    if (cloudsync_uuid_v7(uuid) != 0) return NULL;
    
    return cloudsync_uuid_v7_stringify(uuid, value, dash_format);
}

int cloudsync_uuid_v7_compare (uint8_t value1[UUID_LEN], uint8_t value2[UUID_LEN]) {
    // reconstruct the timestamp by reversing the bit shifts and combining the bytes
    uint64_t t1 = ((uint64_t)value1[0] << 40) | ((uint64_t)value1[1] << 32) | ((uint64_t)value1[2] << 24) | ((uint64_t)value1[3] << 16) | ((uint64_t)value1[4] << 8)  | ((uint64_t)value1[5]);
    uint64_t t2 = ((uint64_t)value2[0] << 40) | ((uint64_t)value2[1] << 32) | ((uint64_t)value2[2] << 24) | ((uint64_t)value2[3] << 16) | ((uint64_t)value2[4] << 8)  | ((uint64_t)value2[5]);
    
    if (t1 == t2) return memcmp(value1, value2, UUID_LEN);
    return (t1 > t2) ? 1 : -1;
}

// MARK: - General -

void *cloudsync_memory_zeroalloc (uint64_t size) {
    void *ptr = (void *)cloudsync_memory_alloc((sqlite3_uint64)size);
    if (!ptr) return NULL;
    
    memset(ptr, 0, (size_t)size);
    return ptr;
}

char *cloudsync_string_dup (const char *str, bool lowercase) {
    if (str == NULL) return NULL;
    
    size_t len = strlen(str);
    char *s = (char *)cloudsync_memory_alloc((sqlite3_uint64)(len + 1));
    if (!s) return NULL;
    
    if (lowercase) {
        // convert each character to lowercase and copy it to the new string
        for (size_t i = 0; i < len; i++) {
            s[i] = tolower(str[i]);
        }
    } else {
        memcpy(s, str, len);
    }
    
    // null-terminate the string
    s[len] = '\0';
    
    return s;
}

void cloudsync_rowid_decode (sqlite3_int64 rowid, sqlite3_int64 *db_version, sqlite3_int64 *seq) {
    // use unsigned 64-bit integer for intermediate calculations
    // when db_version is large enough, it can cause overflow, leading to negative values
    // to handle this correctly, we need to ensure the calculations are done in an unsigned 64-bit integer context
    // before converting back to sqlite3_int64 as needed
    uint64_t urowid = (uint64_t)rowid;
    
    // define the bit mask for seq (30 bits)
    const uint64_t SEQ_MASK = 0x3FFFFFFF; // (2^30 - 1)

    // extract seq by masking the lower 30 bits
    *seq = (sqlite3_int64)(urowid & SEQ_MASK);

    // extract db_version by shifting 30 bits to the right
    *db_version = (sqlite3_int64)(urowid >> 30);
}

uint64_t fnv1a_hash(const char *data, size_t len) {
    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint8_t)data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

// MARK: - CRDT algos -

table_algo crdt_algo_from_name (const char *algo_name) {
    if (algo_name == NULL) return table_algo_none;
    
    if ((strcasecmp(algo_name, "CausalLengthSet") == 0) || (strcasecmp(algo_name, "cls") == 0)) return table_algo_crdt_cls;
    if ((strcasecmp(algo_name, "GrowOnlySet") == 0) || (strcasecmp(algo_name, "gos") == 0)) return table_algo_crdt_gos;
    if ((strcasecmp(algo_name, "DeleteWinsSet") == 0) || (strcasecmp(algo_name, "dws") == 0)) return table_algo_crdt_dws;
    if ((strcasecmp(algo_name, "AddWinsSet") == 0) || (strcasecmp(algo_name, "aws") == 0)) return table_algo_crdt_aws;
    
    // if nothing is found
    return table_algo_none;
}

const char *crdt_algo_name (table_algo algo) {
    switch (algo) {
        case table_algo_crdt_cls: return "cls";
        case table_algo_crdt_gos: return "gos";
        case table_algo_crdt_dws: return "dws";
        case table_algo_crdt_aws: return "aws";
        case table_algo_none: return NULL;
    }
    return NULL;
}

// MARK: - Memory Debugger -

#if CLOUDSYNC_DEBUG_MEMORY
#include <execinfo.h>
#include <inttypes.h>
#include <assert.h>

#include "khash.h"
KHASH_MAP_INIT_INT64(HASHTABLE_INT64_VOIDPTR, void*)

#define STACK_DEPTH             128
#define BUILD_ERROR(...)        char current_error[1024]; snprintf(current_error, sizeof(current_error), __VA_ARGS__)
#define BUILD_STACK(v1,v2)      size_t v1; char **v2 = _ptr_stacktrace(&v1)

typedef struct {
    void        *ptr;
    size_t      size;
    bool        deleted;
    size_t      nrealloc;

    // record where it has been allocated/reallocated
    size_t      nframe;
    char        **frames;

    // record where it has been freed
    size_t      nframe2;
    char        **frames2;
} mem_slot;

static void memdebug_report (char *str, char **stack, size_t nstack, mem_slot *slot);

static khash_t(HASHTABLE_INT64_VOIDPTR) *htable;
static uint64_t nalloc, nrealloc, nfree, mem_current, mem_max;

static void *_ptr_lookup (void *ptr) {
    khiter_t k = kh_get(HASHTABLE_INT64_VOIDPTR, htable, (int64_t)ptr);
    void *result = (k == kh_end(htable)) ? NULL : (void *)kh_value(htable, k);
    return result;
}

static bool _ptr_insert (void *ptr, mem_slot *slot) {
    int err = 0;
    khiter_t k = kh_put(HASHTABLE_INT64_VOIDPTR, htable, (int64_t)ptr, &err);
    if (err != -1) kh_value(htable, k) = (void *)slot;
    return (err != -1);
}

static char **_ptr_stacktrace (size_t *nframes) {
    #if _WIN32
    // http://www.codeproject.com/Articles/11132/Walking-the-callstack
    // https://spin.atomicobject.com/2013/01/13/exceptions-stack-traces-c/
    #else
    void *callstack[STACK_DEPTH];
    int n = backtrace(callstack, STACK_DEPTH);
    char **strs = backtrace_symbols(callstack, n);
    *nframes = (size_t)n;
    return strs;
    #endif
}

static mem_slot *_ptr_add (void *ptr, size_t size) {
    mem_slot *slot = (mem_slot *)calloc(1, sizeof(mem_slot));
    assert(slot);
    
    slot->ptr = ptr;
    slot->size = size;
    slot->frames = _ptr_stacktrace(&slot->nframe);
    bool ok = _ptr_insert(ptr, slot);
    assert(ok);

    ++nalloc;
    mem_current += size;
    if (mem_current > mem_max) mem_max = mem_current;
    
    return slot;
}

static void _ptr_remove (void *ptr) {
    mem_slot *slot = (mem_slot *)_ptr_lookup(ptr);
    if (!slot) {
        BUILD_ERROR("Unable to find old pointer to free.");
        memdebug_report(current_error, NULL, 0, NULL);
        return;
    }
    
    if (slot->deleted) {
        BUILD_ERROR("Pointer already freed.");
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, slot);
    }
    
    size_t old_size = slot->size;
    slot->deleted = true;
    slot->frames2 = _ptr_stacktrace(&slot->nframe2);
    
    ++nfree;
    mem_current -= old_size;
}

static void _ptr_replace (void *old_ptr, void *new_ptr, size_t new_size) {
    if (old_ptr == NULL) {
        _ptr_add(new_ptr, new_size);
        return;
    }
    
    // remove old ptr (implicit free performed by realloc)
    _ptr_remove(old_ptr);
    
    // add newly allocated prt (implicit alloc performed by realloc)
    mem_slot *slot = _ptr_add(new_ptr, new_size);
    ++slot->nrealloc;
    
    ++nrealloc;
    if (mem_current > mem_max) mem_max = mem_current;
}

// MARK: -

static bool stacktrace_is_internal(const char *s) {
    static const char *reserved[] = {"??? ", "libdyld.dylib ", "memdebug_", "_ptr_", NULL};

    const char **r = reserved;
    while (*r) {
        if (strstr(s, *r)) return true;
        ++r;
    }
    return false;
}

static void memdebug_report (char *str, char **stack, size_t nstack, mem_slot *slot) {
    printf("%s\n", str);
    for (size_t i=0; i<nstack; ++i) {
        if (stacktrace_is_internal(stack[i])) continue;
        printf("%s\n", stack[i]);
    }

    if (slot) {
        printf("\nallocated:\n");
        for (size_t i=0; i<slot->nframe; ++i) {
            if (stacktrace_is_internal(slot->frames[i])) continue;
            printf("%s\n", slot->frames[i]);
        }

        printf("\nfreed:\n");
        for (size_t i=0; i<slot->nframe2; ++i) {
            if (stacktrace_is_internal(slot->frames2[i])) continue;
            printf("%s\n", slot->frames2[i]);
        }
    }
}

void memdebug_init (int once) {
    if (htable == NULL) htable = kh_init(HASHTABLE_INT64_VOIDPTR);
}

void memdebug_finalize (void) {
    printf("\n========== MEMORY STATS ==========\n");
    printf("Allocations count: %" PRIu64 "\n", nalloc);
    printf("Reallocations count: %" PRIu64 "\n", nrealloc);
    printf("Free count: %" PRIu64 "\n", nfree);
    printf("Leaked: %" PRIu64 " (bytes)\n", mem_current);
    printf("Max memory usage: %" PRIu64 " (bytes)\n", mem_max);
    printf("==================================\n\n");

    if (mem_current > 0) {
        printf("\n========== LEAKS DETAILS ==========\n");
        
        khiter_t k;
        for (k = kh_begin(htable); k != kh_end(htable); ++k) {
            if (kh_exist(htable, k)) {
                mem_slot *slot = (mem_slot *)kh_value(htable, k);
                if ((!slot->ptr) || (slot->deleted)) continue;
                
                printf("Block %p size: %zu (reallocated %zu)\n", slot->ptr, slot->size, slot->nrealloc);
                printf("Call stack:\n");
                printf("===========\n");
                for (size_t j=0; j<slot->nframe; ++j) {
                    if (stacktrace_is_internal(slot->frames[j])) continue;
                    printf("%s\n", slot->frames[j]);
                }
                printf("===========\n\n");
            }
        }
    }
}

void *memdebug_alloc (sqlite3_uint64 size) {
    void *ptr = sqlite3_malloc64(size);
    if (!ptr) {
        BUILD_ERROR("Unable to allocated a block of %lld bytes", size);
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, NULL);
        return NULL;
    }
    _ptr_add(ptr, size);
    return ptr;
}

void *memdebug_realloc (void *ptr, sqlite3_uint64 new_size) {
    if (!ptr) return memdebug_alloc(new_size);
    
    mem_slot *slot = _ptr_lookup(ptr);
    if (!slot) {
        BUILD_ERROR("Pointer being reallocated was now previously allocated.");
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, NULL);
        return NULL;
    }
    
    void *back_ptr = ptr;
    void *new_ptr = realloc(ptr, new_size);
    if (!new_ptr) {
        BUILD_ERROR("Unable to reallocate a block of %lld bytes.", new_size);
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, slot);
        return NULL;
    }
    
    _ptr_replace(back_ptr, new_ptr, new_size);
    return new_ptr;
}

char *memdebug_vmprintf (const char *format, va_list list) {
    char *ptr = sqlite3_vmprintf(format, list);
    if (!ptr) {
        BUILD_ERROR("Unable to allocated for sqlite3_vmprintf with format %s", format);
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, NULL);
        return NULL;
    }
    
    _ptr_add(ptr, sqlite3_msize(ptr));
    return ptr;
}

char *memdebug_mprintf(const char *format, ...) {
    va_list ap;
    char *z;
    
    va_start(ap, format);
    z = memdebug_vmprintf(format, ap);
    va_end(ap);
    
    return z;
}

sqlite3_uint64 memdebug_msize (void *ptr) {
    return sqlite3_msize(ptr);
}

void memdebug_free (void *ptr) {
    if (!ptr) {
        BUILD_ERROR("Trying to deallocate a NULL ptr.");
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, NULL);
    }
    
    // ensure ptr has been previously allocated by malloc, calloc or realloc and not yet freed with free
    mem_slot *slot = _ptr_lookup(ptr);
    
    if (!slot) {
        BUILD_ERROR("Pointer being freed was not previously allocated.");
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, NULL);
        return;
    }
    
    if (slot->deleted) {
        BUILD_ERROR("Pointer already freed.");
        BUILD_STACK(n, stack);
        memdebug_report(current_error, stack, n, slot);
        return;
    }
    
    _ptr_remove(ptr);
    sqlite3_free(ptr);
}

#endif
