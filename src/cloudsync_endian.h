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

// Only an unconditional byte swap is provided. Wire formats are defined on byte values,
// never on the host's byte order, so the same bytes are produced on every architecture;
// host-order conversions are deliberately absent. `make unittest-s390x` checks this on a
// big-endian host.

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

#endif

