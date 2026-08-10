#!/bin/bash
# the native compiler must render each corpus program exactly as stage 1
# (the host-run compiler) does.
cd "$(dirname "$0")/.."
pass=0; fail=0
for f in corpus/*.art; do
    base=$(basename "$f" .art)
    ./native_compiler "$f" "/tmp/nat_$base" >/dev/null 2>&1
    arturo src/compiler.art "$f" "/tmp/stg1_$base" >/dev/null 2>&1
    if diff -q "/tmp/nat_$base" "/tmp/stg1_$base" >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1)); echo "MISMATCH $base"
    fi
done
echo "== native==stage1: pass=$pass fail=$fail =="
