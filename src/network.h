//
//  network.h
//  cloudsync
//
//  Created by Marco Bambini on 12/12/24.
//

#ifndef __CLOUDSYNC_NETWORK__
#define __CLOUDSYNC_NETWORK__

#include "cloudsync.h"

int cloudsync_network_register (sqlite3 *db, char **pzErrMsg, void *ctx);

#endif
