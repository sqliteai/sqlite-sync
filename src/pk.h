//
//  pk.h
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#ifndef __CLOUDSYNC_PK__
#define __CLOUDSYNC_PK__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "database.h"

typedef int (*pk_decode_callback) (void *xdata, int index, int type, int64_t ival, double dval, char *pval);

extern char * const PRIKEY_NULL_CONSTRAINT_ERROR;

char  *pk_encode_prikey (dbvalue_t **argv, int argc, char *b, size_t *bsize);
char  *pk_encode_value (dbvalue_t *value, size_t *bsize);
char  *pk_encode (dbvalue_t **argv, int argc, char *b, bool is_prikey, size_t *bsize, int skip_idx);
int    pk_decode_prikey (char *buffer, size_t blen, pk_decode_callback cb, void *xdata);
int    pk_decode (char *buffer, size_t blen, int count, size_t *seek, int skip_decode_idx, pk_decode_callback cb, void *xdata);
int    pk_decode_bind_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval);
int    pk_decode_print_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval);
size_t pk_encode_size (dbvalue_t **argv, int argc, int reserved, int skip_idx);
uint64_t pk_checksum (const char *buffer, size_t blen);

#endif
