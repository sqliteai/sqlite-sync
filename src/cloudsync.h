//
//  cloudsync.h
//  cloudsync
//
//  Created by Marco Bambini on 16/05/24.
//

#ifndef __CLOUDSYNC__
#define __CLOUDSYNC__

#include <stdint.h>
#include <stdbool.h>
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#define CLOUDSYNC_VERSION                       "0.8.8"

int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);

#endif
