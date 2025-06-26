#ifdef SQLITE_WASM_EXTRA_INIT

#include "sqlite3.h"
#include <stdio.h>

#include "utils.c"
#include "network.c"
#include "dbutils.c"
#include "cloudsync.c"
#include "vtab.c"
#include "pk.c"
#include "lz4.c"
//ciao
int sqlite3_wasm_extra_init(const char *z) {
    fprintf(stderr, "%s: %s()\n", __FILE__, __func__);
    return sqlite3_auto_extension((void *) sqlite3_cloudsync_init);
}

#endif