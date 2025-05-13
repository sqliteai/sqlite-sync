//
//  vtab.h
//  cloudsync
//
//  Created by Marco Bambini on 23/09/24.
//

#ifndef __CLOUDSYNC_VTAB__
#define __CLOUDSYNC_VTAB__

#include "cloudsync.h"

int cloudsync_vtab_register_changes (sqlite3 *db, cloudsync_context *xdata);
cloudsync_context *cloudsync_vtab_get_context (sqlite3_vtab *vtab);
int cloudsync_vtab_set_error (sqlite3_vtab *vtab, const char *format, ...);

#endif
