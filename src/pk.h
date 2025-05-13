//
//  pk.h
//  cloudsync
//
//  Created by Marco Bambini on 21/08/24.
//

#ifndef __CLOUDSYNC_PK__
#define __CLOUDSYNC_PK__

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

char *pk_encode_prikey (sqlite3_value **argv, int argc, char *b, size_t *bsize);
char *pk_encode (sqlite3_value **argv, int argc, char *b, bool is_prikey, size_t *bsize);
int pk_decode_prikey (char *buffer, size_t blen, int (*cb) (void *xdata, int index, int type, int64_t ival, double dval, char *pval), void *xdata);
int pk_decode(char *buffer, size_t blen, int count, size_t *seek, int (*cb) (void *xdata, int index, int type, int64_t ival, double dval, char *pval), void *xdata);
int pk_decode_bind_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval);
int pk_decode_print_callback (void *xdata, int index, int type, int64_t ival, double dval, char *pval);
size_t pk_encode_size (sqlite3_value **argv, int argc, int reserved);

#endif
