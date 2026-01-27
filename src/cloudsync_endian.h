//
//  cloudsync_endian.h
//  cloudsync
//
//  Created by Marco Bambini on 17/01/26.
//

#ifndef __CLOUDSYNC_ENDIAN__
#define __CLOUDSYNC_ENDIAN__

#include <stdint.h>

#if defined(_MSC_VER)
  #include <stdlib.h>   // _byteswap_uint64
#endif

// =======================================================
//  bswap64 - portable
// =======================================================

static inline uint64_t bswap64_u64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);

#elif defined(__has_builtin)
  #if __has_builtin(__builtin_bswap64)
    return __builtin_bswap64(v);
  #else
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) <<  8) |
           ((v & 0x000000FF00000000ull) >>  8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
  #endif

#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);

#else
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) <<  8) |
           ((v & 0x000000FF00000000ull) >>  8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
#endif
}

// =======================================================
//  Compile-time endianness detection
// =======================================================

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
  #if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #define HOST_IS_LITTLE_ENDIAN 1
  #elif (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define HOST_IS_LITTLE_ENDIAN 0
  #endif
#endif

// WebAssembly is currently defined as little-endian in all major toolchains
#if !defined(HOST_IS_LITTLE_ENDIAN) && (defined(__wasm__) || defined(__EMSCRIPTEN__))
  #define HOST_IS_LITTLE_ENDIAN 1
#endif

// Runtime fallback if unknown at compile-time
static inline int host_is_little_endian_runtime (void) {
    const uint16_t x = 1;
    return *((const uint8_t*)&x) == 1;
}

// =======================================================
//  Public API
// =======================================================

static inline uint64_t host_to_be64 (uint64_t v) {
#if defined(HOST_IS_LITTLE_ENDIAN)
  #if HOST_IS_LITTLE_ENDIAN
    return bswap64_u64(v);
  #else
    return v;
  #endif
#else
    return host_is_little_endian_runtime() ? bswap64_u64(v) : v;
#endif
}

static inline uint64_t be64_to_host (uint64_t v) {
    // same operation (bswap if little-endian)
    return host_to_be64(v);
}

#endif

