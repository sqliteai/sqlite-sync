//
//  cloudsync_sqlite.h
//  cloudsync
//
//  Created by Marco Bambini on 05/12/25.
//

#ifndef __CLOUDSYNC_SQLITE__
#define __CLOUDSYNC_SQLITE__

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

int sqlite3_cloudsync_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);

#endif
