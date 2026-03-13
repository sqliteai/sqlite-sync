//
//  pk.c
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#include "pk.h"
#include "utils.h"
#include "cloudsync_endian.h"
#include "cloudsync.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>
 
/*
 
 The pk_encode and pk_decode functions are designed to serialize and deserialize an array of values (sqlite_value structures)
 into a binary format that can be transmitted over a network or stored efficiently.
 These functions support all the data types natively supported by SQLite (integer, float, blob, text, and null)
 and ensure that the serialized data is platform-independent, particularly with respect to endianess.
 
 pk_encode
 =========
 The pk_encode function encodes an array of values into a contiguous memory buffer.
 This buffer can then be sent over a network or saved to a file, ensuring that the data can be reliably reconstructed later, regardless of the platform.
 
 Algorithm:

 * Number of Columns: The first byte of the buffer stores the number of columns (num_args), which is limited to 255 columns.
 * Type and Length Encoding: For each column:
    * The type of the column (e.g., integer, float, text) is encoded in a single byte. The first 3 bits represent the type, and the remaining 5 bits encode the number of bytes required for the integer or length information if applicable.
    * If the column is an integer or a blob/text type, additional bytes are written to the buffer to store the actual value or the length of the data.
    * Endianess handling is applied using htonl/htonll to ensure integers and floating-point numbers are consistently stored in big-endian format (network byte order), making the serialized data platform-independent.
    * Floating-point numbers are treated as 64-bit integers for endianess conversion.
 * Efficient Storage: By using only the minimum number of bytes required to represent integers and lengths, the solution optimizes storage space, reducing the size of the serialized buffer.
 
 Advantages:

 * Platform Independence: By converting all integers and floating-point values to network byte order, the serialized data can be transmitted between systems with different endianess.
 * Efficiency: The function encodes data into the smallest possible format, minimizing the memory footprint of the serialized data. This is particularly important for network transmission and storage.
 * Flexibility: Supports multiple data types (integer, float, text, blob, null) and variable-length data, making it suitable for a wide range of applications.
 
 pk_decode
 =========
 The pk_decode function decodes the buffer created by pk_encode back into an array of sqlite_value structures.
 This allows the original data to be reconstructed and used by the application.
 
 Algorithm:

 * Read Number of Columns: The function starts by reading the first byte to determine the number of columns in the buffer.
 * Type and Length Decoding: For each column:
    * The function reads the type byte to determine the column's data type and the number of bytes used to store length or integer values.
    * Depending on the type, the appropriate number of bytes is read from the buffer to reconstruct the integer, floating-point value, blob, or text data.
    * Endianess is handled by converting from network byte order back to the host's byte order using ntohl/ntohll.
 * Memory Management: For blob and text data, memory is dynamically allocated to store the decoded data. The caller is responsible for freeing this memory after use.
 
 Advantages:

 * Correctness: By reversing the serialization process, the unpack_columns function ensures that the original data can be accurately reconstructed.
 * Endianess Handling: The function handles endianess conversion during decoding, ensuring that data is correctly interpreted regardless of the platform on which it was serialized or deserialized.
 * Robustness: The function includes error handling to manage cases where the buffer is malformed or insufficient data is available, reducing the risk of corruption or crashes.
 
 Overall Advantages of the Solution

 * Portability: The serialized format is platform-independent, ensuring data can be transmitted across different architectures without compatibility issues.
 * Efficiency: The use of compact encoding for integers and lengths reduces the size of the serialized data, optimizing it for storage and transmission.
 * Versatility: The ability to handle multiple data types and variable-length data makes this solution suitable for complex data structures.
 * Simplicity: The functions are designed to be straightforward to use, with clear memory management responsibilities.
 
 Notes
 
 * Floating point values are encoded as IEEE754 double, 64-bit, big-endian byte order.
 
 */

// Three bits are reserved for the type field, so only values in the 0..7 range can be used (8 values)
// SQLITE already reserved values from 1 to 5
// #define SQLITE_INTEGER                   1   // now DBTYPE_INTEGER
// #define SQLITE_FLOAT                     2   // now DBTYPE_FLOAT
// #define SQLITE_TEXT                      3   // now DBTYPE_TEXT
// #define SQLITE_BLOB                      4   // now DBTYPE_BLOB
// #define SQLITE_NULL                      5   // now DBTYPE_NULL
#define DATABASE_TYPE_NEGATIVE_INTEGER      0   // was SQLITE_NEGATIVE_INTEGER
#define DATABASE_TYPE_MAX_NEGATIVE_INTEGER  6   // was SQLITE_MAX_NEGATIVE_INTEGER
#define DATABASE_TYPE_NEGATIVE_FLOAT        7   // was SQLITE_NEGATIVE_FLOAT

char * const PRIKEY_NULL_CONSTRAINT_ERROR = "PRIKEY_NULL_CONSTRAINT_ERROR";

// MARK: - Public Callbacks -

int pk_decode_bind_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    // default decode callback used to bind values to a dbvm_t vm
    
    int rc = DBRES_OK;
    switch (type) {
        case DBTYPE_INTEGER:
            rc = databasevm_bind_int(xdata, index+1, ival);
            break;
        
        case DBTYPE_FLOAT:
            rc = databasevm_bind_double(xdata, index+1, dval);
            break;
            
        case DBTYPE_NULL:
            rc = databasevm_bind_null(xdata, index+1);
            break;
            
        case DBTYPE_TEXT:
            rc = databasevm_bind_text(xdata, index+1, pval, (int)ival);
            break;
            
        case DBTYPE_BLOB:
            rc = databasevm_bind_blob(xdata, index+1, (const void *)pval, ival);
            break;
    }
    
    return rc;
}

int pk_decode_print_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    switch (type) {
        case DBTYPE_INTEGER:
            printf("%d\tINTEGER:\t%" PRId64 "\n", index, ival);
            break;
        
        case DBTYPE_FLOAT:
            printf("%d\tFLOAT:\t%.5f\n", index, dval);
            break;
            
        case DBTYPE_NULL:
            printf("%d\tNULL\n", index);
            break;
            
        case DBTYPE_TEXT:
            printf("%d\tTEXT:\t%.*s\n", index, (int)ival, pval);
            break;
            
        case DBTYPE_BLOB:
            printf("%d\tBLOB:\t%" PRId64 " bytes\n", index, ival);
            break;
    }
    
    return DBRES_OK;
}

uint64_t pk_checksum (const char *buffer, size_t blen) {
    const uint8_t *p = (const uint8_t *)buffer;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < blen; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// MARK: - Decoding -

static inline int pk_decode_check_bounds (size_t bseek, size_t blen, size_t need) {
    // bounds check helper for decoding
    if (bseek > blen) return 0;
    return need <= (blen - bseek);
}

int pk_decode_u8 (const uint8_t *buffer, size_t blen, size_t *bseek, uint8_t *out) {
    if (!pk_decode_check_bounds(*bseek, blen, 1)) return 0;
    *out = buffer[*bseek];
    *bseek += 1;
    return 1;
}

static int pk_decode_uint64 (const uint8_t *buffer, size_t blen, size_t *bseek, size_t nbytes, uint64_t *out) {
    if (nbytes > 8) return 0;
    if (!pk_decode_check_bounds(*bseek, blen, nbytes)) return 0;
    
    // decode bytes in big-endian order (most significant byte first)
    uint64_t v = 0;
    for (size_t i = 0; i < nbytes; i++) {
        v = (v << 8) | (uint64_t)buffer[*bseek];
        (*bseek)++;
    }
    
    *out = v;
    return 1;
}

static int pk_decode_data (const uint8_t *buffer, size_t blen, size_t *bseek, size_t n, const uint8_t **out) {
    if (!pk_decode_check_bounds(*bseek, blen, n)) return 0;
    *out = buffer + *bseek;
    *bseek += n;
    
    return 1;
}

int pk_decode_double (const uint8_t *buffer, size_t blen, size_t *bseek, double *out) {
    // Doubles are encoded as IEEE754 64-bit, big-endian.
    // Convert back to host order before memcpy into double.
    
    uint64_t bits_be = 0;
    if (!pk_decode_uint64(buffer, blen, bseek, sizeof(uint64_t), &bits_be)) return 0;
    
    uint64_t bits = be64_to_host(bits_be);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(bits));
    *out = value;
    return 1;
}

int pk_decode (char *buffer, size_t blen, int count, size_t *seek, int skip_decode_idx, pk_decode_callback cb, void *xdata) {
    const uint8_t *ubuf = (const uint8_t *)buffer;
    size_t bseek = (seek) ? *seek : 0;
    if (count == -1) {
        uint8_t c = 0;
        if (!pk_decode_u8(ubuf, blen, &bseek, &c)) return -1;
        count = (int)c;
    }
        
    for (size_t i = 0; i < (size_t)count; i++) {
        uint8_t type_byte = 0;
        if (!pk_decode_u8(ubuf, blen, &bseek, &type_byte)) return -1;
        int raw_type = (int)(type_byte & 0x07);
        size_t nbytes = (size_t)((type_byte >> 3) & 0x1F);

        // skip_decode wants the raw encoded slice (type_byte + optional len/int + payload)
        // we still must parse with the *raw* type to know how much to skip
        bool skip_decode = ((skip_decode_idx >= 0) && (i == (size_t)skip_decode_idx));
        size_t initial_bseek = bseek - 1; // points to type_byte

        switch (raw_type) {
            case DATABASE_TYPE_MAX_NEGATIVE_INTEGER: {
                // must not carry length bits
                if (nbytes != 0) return -1;
                if (skip_decode) {
                    size_t slice_len = bseek - initial_bseek;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_BLOB, (int64_t)slice_len, 0.0, (char *)(buffer + initial_bseek)) != DBRES_OK) return -1;
                } else {
                    int64_t value = INT64_MIN;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_INTEGER, value, 0.0, NULL) != DBRES_OK) return -1;
                }
            }
                break;
                
            case DATABASE_TYPE_NEGATIVE_INTEGER:
            case DBTYPE_INTEGER: {
                // validate nbytes to avoid UB/overreads
                if (nbytes < 1 || nbytes > 8) return -1;
                uint64_t u = 0;
                if (!pk_decode_uint64(ubuf, blen, &bseek, nbytes, &u)) return -1;
                
                if (skip_decode) {
                    size_t slice_len = bseek - initial_bseek;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_BLOB, (int64_t)slice_len, 0.0, (char *)(buffer + initial_bseek)) != DBRES_OK) return -1;
                } else {
                    int64_t value = (int64_t)u;
                    if (raw_type == DATABASE_TYPE_NEGATIVE_INTEGER) value = -value;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_INTEGER, value, 0.0, NULL) != DBRES_OK) return -1;
                }
            }
                break;
                
            case DATABASE_TYPE_NEGATIVE_FLOAT:
            case DBTYPE_FLOAT: {
                // encoder stores float type with no length bits, so enforce nbytes==0
                if (nbytes != 0) return -1;
                double value = 0.0;
                if (!pk_decode_double(ubuf, blen, &bseek, &value)) return -1;
                
                if (skip_decode) {
                    size_t slice_len = bseek - initial_bseek;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_BLOB, (int64_t)slice_len, 0.0, (char *)(buffer + initial_bseek)) != DBRES_OK) return -1;
                } else {
                    if (raw_type == DATABASE_TYPE_NEGATIVE_FLOAT) value = -value;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_FLOAT, 0, value, NULL) != DBRES_OK) return -1;
                }
            }
                break;
                
            case DBTYPE_TEXT:
            case DBTYPE_BLOB: {
                // validate nbytes for length field
                if (nbytes < 1 || nbytes > 8) return -1;
                uint64_t ulen = 0;
                if (!pk_decode_uint64(ubuf, blen, &bseek, nbytes, &ulen)) return -1;
                
                // ensure ulen fits in size_t on this platform
                if (ulen > (uint64_t)SIZE_MAX) return -1;
                size_t len = (size_t)ulen;
                const uint8_t *p = NULL;
                if (!pk_decode_data(ubuf, blen, &bseek, len, &p)) return -1;
                
                if (skip_decode) {
                    // return the full encoded slice (type_byte + len bytes + payload)
                    size_t slice_len = bseek - initial_bseek;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_BLOB, (int64_t)slice_len, 0.0, (char *)(buffer + initial_bseek)) != DBRES_OK) return -1;
                } else {
                    if (cb) if (cb(xdata, (int)i, raw_type, (int64_t)len, 0.0, (char *)p) != DBRES_OK) return -1;
                }
            }
                break;
                
            case DBTYPE_NULL: {
                if (nbytes != 0) return -1;
                if (skip_decode) {
                    size_t slice_len = bseek - initial_bseek;
                    if (cb) if (cb(xdata, (int)i, DBTYPE_BLOB, (int64_t)slice_len, 0.0, (char *)(buffer + initial_bseek)) != DBRES_OK) return -1;
                } else {
                    if (cb) if (cb(xdata, (int)i, DBTYPE_NULL, 0, 0.0, NULL) != DBRES_OK) return -1;
                }
            }
                break;
            
            default:
                // should never reach this point
                return -1;
        }
    }
    
    if (seek) *seek = bseek;
    return count;
}

int pk_decode_prikey (char *buffer, size_t blen, pk_decode_callback cb, void *xdata) {
    const uint8_t *ubuf = (const uint8_t *)buffer;
    size_t bseek = 0;
    uint8_t count = 0;
    if (!pk_decode_u8(ubuf, blen, &bseek, &count)) return -1;
    return pk_decode(buffer, blen, count, &bseek, -1, cb, xdata);
}

// MARK: - Encoding -

size_t pk_encode_nbytes_needed (int64_t value) {
    uint64_t v = (uint64_t)value;
    if (v <= 0xFFULL) return 1;
    if (v <= 0xFFFFULL) return 2;
    if (v <= 0xFFFFFFULL) return 3;
    if (v <= 0xFFFFFFFFULL) return 4;
    if (v <= 0xFFFFFFFFFFULL) return 5;
    if (v <= 0xFFFFFFFFFFFFULL) return 6;
    if (v <= 0xFFFFFFFFFFFFFFULL) return 7;
    return 8;
}

static inline int pk_encode_add_overflow_size (size_t a, size_t b, size_t *out) {
    // safe size_t addition helper (prevents overflow)
    if (b > (SIZE_MAX - a)) return 1;
    *out = a + b;
    return 0;
}

size_t pk_encode_size (dbvalue_t **argv, int argc, int reserved, int skip_idx) {
    // estimate the required buffer size
    size_t required = reserved;
    size_t nbytes;
    int64_t val;
    
    for (int i = 0; i < argc; i++) {
        switch (database_value_type(argv[i])) {
            case DBTYPE_INTEGER: {
                val = database_value_int(argv[i]);
                if (val == INT64_MIN) {
                    if (pk_encode_add_overflow_size(required, 1, &required)) return SIZE_MAX;
                    break;
                }
                if (val < 0) val = -val;
                nbytes = pk_encode_nbytes_needed(val);
                
                size_t tmp = 0;
                if (pk_encode_add_overflow_size(1, nbytes, &tmp)) return SIZE_MAX;
                if (pk_encode_add_overflow_size(required, tmp, &required)) return SIZE_MAX;
            } break;
                
            case DBTYPE_FLOAT: {
                size_t tmp = 0;
                if (pk_encode_add_overflow_size(1, sizeof(uint64_t), &tmp)) return SIZE_MAX;
                if (pk_encode_add_overflow_size(required, tmp, &required)) return SIZE_MAX;
            } break;
                
            case DBTYPE_TEXT:
            case DBTYPE_BLOB: {
                size_t len_sz = (size_t)database_value_bytes(argv[i]);
                if (i == skip_idx) {
                    if (pk_encode_add_overflow_size(required, len_sz, &required)) return SIZE_MAX;
                    break;
                }
                
                // Ensure length can be represented by encoder (we encode length with up to 8 bytes)
                // pk_encode_nbytes_needed expects int64-ish values; clamp-check here.
                if (len_sz > (size_t)INT64_MAX) return SIZE_MAX;
                nbytes = pk_encode_nbytes_needed((int64_t)len_sz);
                
                size_t tmp = 0;
                // 1(type) + nbytes(len) + len_sz(payload)
                if (pk_encode_add_overflow_size(1, nbytes, &tmp)) return SIZE_MAX;
                if (pk_encode_add_overflow_size(tmp, len_sz, &tmp)) return SIZE_MAX;
                if (pk_encode_add_overflow_size(required, tmp, &required)) return SIZE_MAX;
            } break;
                
            case DBTYPE_NULL: {
                if (pk_encode_add_overflow_size(required, 1, &required)) return SIZE_MAX;
            } break;
        }
    }
    
    return required;
}

size_t pk_encode_u8 (char *buffer, size_t bseek, uint8_t value) {
    buffer[bseek++] = value;
    return bseek;
}

static size_t pk_encode_uint64 (char *buffer, size_t bseek, uint64_t value, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        buffer[bseek++] = (uint8_t)((value >> (8 * (nbytes - 1 - i))) & 0xFFu);
    }
    return bseek;
}

size_t pk_encode_data (char *buffer, size_t bseek, char *data, size_t datalen) {
    memcpy(buffer + bseek, data, datalen);
    return bseek + datalen;
}
    
char *pk_encode (dbvalue_t **argv, int argc, char *b, bool is_prikey, size_t *bsize, int skip_idx) {
    size_t bseek = 0;
    char *buffer = b;
    
    // always compute blen (even if it is not a primary key)
    size_t blen = pk_encode_size(argv, argc, (is_prikey) ? 1 : 0, skip_idx);
    if (blen == SIZE_MAX) return NULL;
    if (argc < 0) return NULL;
    
    // in primary-key encoding the number of items must be explicitly added to the encoded buffer
    if (is_prikey) {
        if (!bsize) return NULL;
        // must fit in a single byte
        if (argc > 255) return NULL;
        
        // if schema does not enforce NOT NULL on primary keys, check at runtime
        #ifndef CLOUDSYNC_CHECK_NOTNULL_PRIKEYS
        for (int i = 0; i < argc; i++) {
            if (database_value_type(argv[i]) == DBTYPE_NULL) return PRIKEY_NULL_CONSTRAINT_ERROR;
        }
        #endif
        
        // 1 is the number of items in the serialization
        // always 1 byte so max 255 primary keys, even if there is an hard SQLite limit of 128
        size_t blen_curr = *bsize;
        buffer = (blen > blen_curr || b == NULL) ? cloudsync_memory_alloc((uint64_t)blen) : b;
        if (!buffer) return NULL;
        
        // the first u8 value is the total number of items in the primary key(s)
        bseek = pk_encode_u8(buffer, 0, (uint8_t)argc);
    } else {
        // ensure buffer exists and is large enough also in non-prikey mode
        size_t curr = (bsize) ? *bsize : 0;
        if (buffer == NULL || curr < blen) return NULL;
    }
        
    for (int i = 0; i < argc; i++) {
        int type = database_value_type(argv[i]);
        switch (type) {
            case DBTYPE_INTEGER: {
                int64_t value = database_value_int(argv[i]);
                if (value == INT64_MIN) {
                    bseek = pk_encode_u8(buffer, bseek, DATABASE_TYPE_MAX_NEGATIVE_INTEGER);
                    break;
                }
                if (value < 0) {value = -value; type = DATABASE_TYPE_NEGATIVE_INTEGER;}
                size_t nbytes = pk_encode_nbytes_needed(value);
                uint8_t type_byte = (uint8_t)((nbytes << 3) | type);
                bseek = pk_encode_u8(buffer, bseek, type_byte);
                bseek = pk_encode_uint64(buffer, bseek, (uint64_t)value, nbytes);
            }
                break;
            case DBTYPE_FLOAT: {
                // Encode doubles as IEEE754 64-bit, big-endian
                double value = database_value_double(argv[i]);
                if (value < 0) {value = -value; type = DATABASE_TYPE_NEGATIVE_FLOAT;}
                uint64_t bits;
                memcpy(&bits, &value, sizeof(bits));
                bits = host_to_be64(bits);
                bseek = pk_encode_u8(buffer, bseek, (uint8_t)type);
                bseek = pk_encode_uint64(buffer, bseek, bits, sizeof(bits));
            }
                break;
            case DBTYPE_TEXT:
            case DBTYPE_BLOB: {
                size_t len = (size_t)database_value_bytes(argv[i]);
                if (i == skip_idx) {
                    memcpy(buffer + bseek, (char *)database_value_blob(argv[i]), len);
                    bseek += len;
                    break;
                }

                if (len > (size_t)INT64_MAX) return NULL;
                size_t nbytes = pk_encode_nbytes_needed((int64_t)len);
                uint8_t type_byte = (uint8_t)((nbytes << 3) | database_value_type(argv[i]));
                bseek = pk_encode_u8(buffer, bseek, type_byte);
                bseek = pk_encode_uint64(buffer, bseek, (uint64_t)len, nbytes);
                bseek = pk_encode_data(buffer, bseek, (char *)database_value_blob(argv[i]), len);
            }
                break;
            case DBTYPE_NULL: {
                bseek = pk_encode_u8(buffer, bseek, DBTYPE_NULL);
            }
                break;
        }
    }
    
    // return actual bytes written; for prikey it's equal to blen, but safer to report bseek
    if (bsize) *bsize = bseek;
    return buffer;
}

char *pk_encode_prikey (dbvalue_t **argv, int argc, char *b, size_t *bsize) {
    return pk_encode(argv, argc, b, true, bsize, -1);
}

char *pk_encode_value (dbvalue_t *value, size_t *bsize) {
    dbvalue_t *argv[1] = {value};
    
    size_t blen = pk_encode_size(argv, 1, 0, -1);
    if (blen == SIZE_MAX) return NULL;
    
    char *buffer = cloudsync_memory_alloc((uint64_t)blen);
    if (!buffer) return NULL;
    
    *bsize = blen;
    return pk_encode(argv, 1, buffer, false, bsize, -1);
}
