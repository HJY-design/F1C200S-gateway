#!/bin/sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
testdir=$(mktemp -d /tmp/gateway-backend-test.XXXXXX)
mkdir -p "$testdir/include/cjson" "$testdir/logs" "$testdir/audit" "$testdir/history"
cp "$project"/output/build/cjson-*/cJSON.h "$testdir/include/cjson/cJSON.h"
cc -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -I"$testdir/include" \
  -DGW_LOGDIR="\"$testdir/logs\"" -DGW_AUDIT="\"$testdir/audit\"" \
  -DGW_HISTORY="\"$testdir/history\"" -DGW_HISTORY_LOCK="\"$testdir/history.lock\"" \
  "$project/buildroot-external/package/gateway-control/tests/backend-test.c" \
  "$project/buildroot-external/package/gateway-control/src/gw-common.c" \
  "$project"/output/build/cjson-*/cJSON.c -lm -o "$testdir/backend-test"
"$testdir/backend-test"
printf 'Isolated fixtures: %s\n' "$testdir"
