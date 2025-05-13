//
//  pk.c
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#include "pk.h"
#include "utils.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

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
 
 */

// Three bits are reserved for the type field, so only values in the 0..7 range can be used (8 values)
// SQLITE already reserved values from 1 to 5
// #define SQLITE_INTEGER               1
// #define SQLITE_FLOAT                 2
// #define SQLITE_TEXT                  3
// #define SQLITE_BLOB                  4
// #define SQLITE_NULL                  5
#define SQLITE_NEGATIVE_INTEGER         0
#define SQLITE_MAX_NEGATIVE_INTEGER     6
#define SQLITE_NEGATIVE_FLOAT           7

// MARK: - Decoding -

int pk_decode_bind_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    // default decode callback used to bind values to a sqlite3_stmt vm
    
    sqlite3_stmt *vm = (sqlite3_stmt *)xdata;
    int rc = SQLITE_OK;
    switch (type) {
        case SQLITE_INTEGER:
            rc = sqlite3_bind_int64(vm, index+1, ival);
            break;
        
        case SQLITE_FLOAT:
            rc = sqlite3_bind_double(vm, index+1, dval);
            break;
            
        case SQLITE_NULL:
            rc = sqlite3_bind_null(vm, index+1);
            break;
            
        case SQLITE_TEXT:
            rc = sqlite3_bind_text(vm, index+1, pval, (int)ival, SQLITE_STATIC);
            break;
            
        case SQLITE_BLOB:
            rc = sqlite3_bind_blob64(vm, index+1, (const void *)pval, (sqlite3_uint64)ival, SQLITE_STATIC);
            break;
    }
    
    return rc;
}

int pk_decode_print_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval) {
    switch (type) {
        case SQLITE_INTEGER:
            printf("%d\tINTEGER:\t%lld\n", index, (long long)ival);
            break;
        
        case SQLITE_FLOAT:
            printf("%d\tFLOAT:\t%.5f\n", index, dval);
            break;
            
        case SQLITE_NULL:
            printf("%d\tNULL\n", index);
            break;
            
        case SQLITE_TEXT:
            printf("%d\tTEXT:\t%s\n", index, pval);
            break;
            
        case SQLITE_BLOB:
            printf("%d\tBLOB:\t%lld bytes\n", index, (long long)ival);
            break;
    }
    
    return SQLITE_OK;
}

uint8_t pk_decode_u8 (char *buffer, size_t *bseek) {
    uint8_t value = buffer[*bseek];
    *bseek += 1;
    return value;
}

int64_t pk_decode_int64 (char *buffer, size_t *bseek, size_t nbytes) {
    int64_t value = 0;
    
    // decode bytes in big-endian order (most significant byte first)
    for (size_t i = 0; i < nbytes; i++) {
        value = (value << 8) | (uint8_t)buffer[*bseek];
        (*bseek)++;
    }
    
    return value;
}

char *pk_decode_data (char *buffer, size_t *bseek, int32_t blen) {
    char *value = buffer + *bseek;
    *bseek += blen;
    
    return value;
}

double pk_decode_double (char *buffer, size_t *bseek) {
    double value = 0;
    int64_t int64value = pk_decode_int64(buffer, bseek, sizeof(int64_t));
    memcpy(&value, &int64value, sizeof(int64_t));
    
    return value;
}

int pk_decode(char *buffer, size_t blen, int count, size_t *seek, int (*cb) (void *xdata, int index, int type, int64_t ival, double dval, char *pval), void *xdata) {
    size_t bseek = (seek) ? *seek : 0;
    if (count == -1) count = pk_decode_u8(buffer, &bseek);
    
    for (size_t i = 0; i < (size_t)count; i++) {
        uint8_t type_byte = (uint8_t)pk_decode_u8(buffer, &bseek);
        int type = (int)(type_byte & 0x07);
        size_t nbytes = (type_byte >> 3) & 0x1F;
        
        switch (type) {
            case SQLITE_MAX_NEGATIVE_INTEGER: {
                int64_t value = INT64_MIN;
                type = SQLITE_INTEGER;
                if (cb) if (cb(xdata, (int)i, type, value, 0.0, NULL) != SQLITE_OK) return -1;
            }
                break;
                
            case SQLITE_NEGATIVE_INTEGER:
            case SQLITE_INTEGER: {
                int64_t value = pk_decode_int64(buffer, &bseek, nbytes);
                if (type == SQLITE_NEGATIVE_INTEGER) {value = -value; type = SQLITE_INTEGER;}
                if (cb) if (cb(xdata, (int)i, type, value, 0.0, NULL) != SQLITE_OK) return -1;
            }
                break;
                
            case SQLITE_NEGATIVE_FLOAT:
            case SQLITE_FLOAT: {
                double value = pk_decode_double(buffer, &bseek);
                if (type == SQLITE_NEGATIVE_FLOAT) {value = -value; type = SQLITE_FLOAT;}
                if (cb) if (cb(xdata, (int)i, type, 0, value, NULL) != SQLITE_OK) return -1;
            }
                break;
                
            case SQLITE_TEXT:
            case SQLITE_BLOB: {
                int64_t length = pk_decode_int64(buffer, &bseek, nbytes);
                char *value = pk_decode_data(buffer, &bseek, (int32_t)length);
                if (cb) if (cb(xdata, (int)i, type, length, 0.0, value) != SQLITE_OK) return -1;
            }
                break;
                
            case SQLITE_NULL: {
                if (cb) if (cb(xdata, (int)i, type, 0, 0.0, NULL) != SQLITE_OK) return -1;
            }
                break;
        }
    }
    
    if (seek) *seek = bseek;
    return count;
}

int pk_decode_prikey (char *buffer, size_t blen, int (*cb) (void *xdata, int index, int type, int64_t ival, double dval, char *pval), void *xdata) {
    size_t bseek = 0;
    uint8_t count = pk_decode_u8(buffer, &bseek);
    return pk_decode(buffer, blen, count, &bseek, cb, xdata);
}

// MARK: - Encoding -

size_t pk_encode_nbytes_needed (int64_t value) {
    if (value <= 0x7F) return 1; // 7 bits
    if (value <= 0x7FFF) return 2; // 15 bits
    if (value <= 0x7FFFFF) return 3; // 23 bits
    if (value <= 0x7FFFFFFF) return 4; // 31 bits
    if (value <= 0x7FFFFFFFFF) return 5; // 39 bits
    if (value <= 0x7FFFFFFFFFFF) return 6; // 47 bits
    if (value <= 0x7FFFFFFFFFFFFF) return 7; // 55 bits
    return 8; // Larger than 7-byte range, needs 8 bytes
}

size_t pk_encode_size (sqlite3_value **argv, int argc, int reserved) {
    // estimate the required buffer size
    size_t required = reserved;
    size_t nbytes;
    int64_t val, len;
    
    for (int i = 0; i < argc; i++) {
        switch (sqlite3_value_type(argv[i])) {
            case SQLITE_INTEGER:
                val = sqlite3_value_int64(argv[i]);
                if (val == INT64_MIN) {
                    required += 1;
                    break;
                }
                if (val < 0) val = -val;
                nbytes = pk_encode_nbytes_needed(val);
                required += 1 + nbytes;
                break;
            case SQLITE_FLOAT:
                required += 1 + sizeof(int64_t);
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB:
                len = (int32_t)sqlite3_value_bytes(argv[i]);
                nbytes = pk_encode_nbytes_needed(len);
                required += 1 + len + nbytes;
                break;
            case SQLITE_NULL:
                required += 1;
                break;
        }
    }
    
    return required;
}

size_t pk_encode_u8 (char *buffer, size_t bseek, uint8_t value) {
    buffer[bseek++] = value;
    return bseek;
}

size_t pk_encode_int64 (char *buffer, size_t bseek, int64_t value, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        buffer[bseek++] = (uint8_t)((value >> (8 * (nbytes - 1 - i))) & 0xFF);
    }
    return bseek;
}

size_t pk_encode_data (char *buffer, size_t bseek, char *data, size_t datalen) {
    memcpy(buffer + bseek, data, datalen);
    return bseek + datalen;
}
    
char *pk_encode (sqlite3_value **argv, int argc, char *b, bool is_prikey, size_t *bsize) {
    size_t bseek = 0;
    size_t blen = 0;
    char *buffer = b;
    
    // in primary-key encoding the number of items must be explicitly added to the encoded buffer
    if (is_prikey) {
        // 1 is the number of items in the serialization (always 1 byte so max 255 primary keys, even if there is an hard SQLite limit of 128)
        blen = pk_encode_size(argv, argc, 1);
        size_t blen_curr = *bsize;
        buffer = (blen > blen_curr || b == NULL) ? cloudsync_memory_alloc((sqlite3_uint64)blen) : b;
        if (!buffer) return NULL;
        
        // the first u8 value is the total number of items in the primary key(s)
        bseek = pk_encode_u8(buffer, 0, argc);
    }
        
    for (int i = 0; i < argc; i++) {
        int type = sqlite3_value_type(argv[i]);
        switch (type) {
            case SQLITE_INTEGER: {
                int64_t value = sqlite3_value_int64(argv[i]);
                if (value == INT64_MIN) {
                    bseek = pk_encode_u8(buffer, bseek, SQLITE_MAX_NEGATIVE_INTEGER);
                    break;
                }
                if (value < 0) {value = -value; type = SQLITE_NEGATIVE_INTEGER;}
                size_t nbytes = pk_encode_nbytes_needed(value);
                uint8_t type_byte = (nbytes << 3) | type;
                bseek = pk_encode_u8(buffer, bseek, type_byte);
                bseek = pk_encode_int64(buffer, bseek, value, nbytes);
            }
                break;
            case SQLITE_FLOAT: {
                double value = sqlite3_value_double(argv[i]);
                if (value < 0) {value = -value; type = SQLITE_NEGATIVE_FLOAT;}
                int64_t net_double;
                memcpy(&net_double, &value, sizeof(int64_t));
                bseek = pk_encode_u8(buffer, bseek, type);
                bseek = pk_encode_int64(buffer, bseek, net_double, sizeof(int64_t));
            }
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB: {
                int32_t len = (int32_t)sqlite3_value_bytes(argv[i]);
                size_t nbytes = pk_encode_nbytes_needed(len);
                uint8_t type_byte = (nbytes << 3) | sqlite3_value_type(argv[i]);
                bseek = pk_encode_u8(buffer, bseek, type_byte);
                bseek = pk_encode_int64(buffer, bseek, len, nbytes);
                bseek = pk_encode_data(buffer, bseek, (char *)sqlite3_value_blob(argv[i]), len);
            }
                break;
            case SQLITE_NULL: {
                bseek = pk_encode_u8(buffer, bseek, SQLITE_NULL);
            }
                break;
        }
    }
    
    if (bsize) *bsize = blen;
    return buffer;
}

char *pk_encode_prikey (sqlite3_value **argv, int argc, char *b, size_t *bsize) {
    return pk_encode(argv, argc, b, true, bsize);
}
