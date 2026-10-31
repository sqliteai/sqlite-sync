#!/bin/sh
# Run the apply fuzzer and review_regressions under MemorySanitizer in a Linux container
# (MemorySanitizer is not available on macOS). Run from the repository root:
#   ./test/fuzz/msan_docker.sh [iterations]
#
# glibc's getentropy/timespec_get and the fstat/getcwd calls inside SQLite's file code have no
# MemorySanitizer interceptor, so they are replaced or reported as noise: the fuzzer uses
# in-memory databases and must report nothing; for review_regressions, only reports whose
# first frame is outside sqlite3.c matter.
set -e
ITER=${1:-6000}
docker run --rm -v "$PWD":/src -e ITER="$ITER" debian:bookworm bash -c '
apt-get update -qq >/dev/null && apt-get install -y -qq clang >/dev/null
cd /src
rm -rf /tmp/msan && mkdir -p /tmp/msan
REC="-fsanitize=memory -fsanitize-recover=memory -fsanitize-memory-track-origins"
clang -O1 -g $REC -DSQLITE_DQS=0 -DSQLITE_CORE -Isqlite -c sqlite/sqlite3.c -o /tmp/msan/sqlite3.o
cat > /tmp/msan/shim.c <<SHIM
#include <time.h>
#include <stdio.h>
int cs_timespec_get(struct timespec *ts, int base) { return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0; }
int cs_getentropy(void *buf, size_t n) { FILE *f = fopen("/dev/urandom", "rb"); if (!f) return -1; size_t got = fread(buf, 1, n, f); fclose(f); return got == n ? 0 : -1; }
SHIM
SRCS="src/block.c src/cloudsync.c src/dbutils.c src/lz4.c src/pk.c src/utils.c src/sqlite/cloudsync_changes_sqlite.c src/sqlite/cloudsync_sqlite.c src/sqlite/database_sqlite.c src/sqlite/sql_sqlite.c modules/fractional-indexing/fractional_indexing.c"
FL="-Dtimespec_get=cs_timespec_get -Dgetentropy=cs_getentropy -O1 -g -fno-omit-frame-pointer $REC -DSQLITE_CORE -DCLOUDSYNC_OMIT_NETWORK -DCLOUDSYNC_OMIT_PRINT_RESULT -Isrc -Isrc/sqlite -Isrc/network -Isqlite -Imodules/fractional-indexing"
clang $FL test/fuzz/fuzz_apply.c $SRCS /tmp/msan/sqlite3.o /tmp/msan/shim.c -lm -lpthread -ldl -o /tmp/msan/fuzz_apply
clang $FL -DCLOUDSYNC_UNITTEST test/review_regressions.c $SRCS /tmp/msan/sqlite3.o /tmp/msan/shim.c -lm -lpthread -ldl -o /tmp/msan/review_regressions
export MSAN_OPTIONS=halt_on_error=0
/tmp/msan/review_regressions > /tmp/rr.out 2>&1 || true
echo "review_regressions: $(grep -c "^SUMMARY" /tmp/rr.out) reports, $(grep "^SUMMARY" /tmp/rr.out | grep -vc sqlite3.c) outside sqlite3.c"
/tmp/msan/fuzz_apply $ITER 7 > /tmp/fz.out 2>&1 || true
echo "fuzz_apply: $(grep -c "^SUMMARY" /tmp/fz.out) reports"; grep iterations /tmp/fz.out
'
