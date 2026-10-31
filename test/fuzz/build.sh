#!/bin/sh
# Build the apply fuzzer with a sanitizer: ./build.sh address,undefined|memory|thread [out]
# Run from the repository root.
set -e
SAN=${1:-address,undefined}
OUT=${2:-build/fuzz/fuzz_apply}
CC=${CC:-clang}
[ "$(uname)" = Darwin ] && EXTRA_LIBS=${EXTRA_LIBS:--framework Security}
mkdir -p "$(dirname "$OUT")"
OBJ="$(dirname "$OUT")/sqlite3.o"
INC="-Isrc -Isrc/sqlite -Isrc/network -Isqlite -Imodules/fractional-indexing"
[ -f "$OBJ" ] || $CC -O1 -g -DSQLITE_DQS=0 -DSQLITE_CORE -Isqlite -c sqlite/sqlite3.c -o "$OBJ"
$CC -O1 -g -fno-omit-frame-pointer -fsanitize="$SAN" -DSQLITE_CORE -DCLOUDSYNC_OMIT_NETWORK -DCLOUDSYNC_OMIT_PRINT_RESULT $INC \
    test/fuzz/fuzz_apply.c src/block.c src/cloudsync.c src/dbutils.c src/lz4.c src/pk.c src/utils.c \
    src/sqlite/cloudsync_changes_sqlite.c src/sqlite/cloudsync_sqlite.c src/sqlite/database_sqlite.c src/sqlite/sql_sqlite.c \
    modules/fractional-indexing/fractional_indexing.c "$OBJ" -lm -lpthread $EXTRA_LIBS -o "$OUT"
