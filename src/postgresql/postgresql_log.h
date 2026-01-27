//
//  postgresql_log.h
//  cloudsync
//
//  PostgreSQL-specific logging implementation using elog()
//
//  Note: This header requires _POSIX_C_SOURCE and _GNU_SOURCE to be defined
//  before any includes. These are set as compiler flags in Makefile.postgresql.
//

#ifndef __POSTGRESQL_LOG__
#define __POSTGRESQL_LOG__

// setjmp.h is needed before postgres.h for sigjmp_buf type
#include <setjmp.h>

// Include PostgreSQL headers
#include "postgres.h"
#include "utils/elog.h"

// PostgreSQL logging macros using elog()
// DEBUG1 is the highest priority debug level in PostgreSQL
// LOG is for informational messages
#define CLOUDSYNC_LOG_DEBUG(...)    elog(DEBUG1, __VA_ARGS__)
#define CLOUDSYNC_LOG_INFO(...)     elog(LOG, __VA_ARGS__)

#endif
