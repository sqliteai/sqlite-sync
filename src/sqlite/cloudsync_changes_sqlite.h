//
//  cloudsync_changes_sqlite.h
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#ifndef __CLOUDSYNC_CHANGES_SQLITE__
#define __CLOUDSYNC_CHANGES_SQLITE__

#include "../cloudsync.h"

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

int cloudsync_vtab_register_changes (sqlite3 *db, cloudsync_context *xdata);

#endif
