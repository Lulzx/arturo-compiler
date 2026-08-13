#!/bin/bash
# Deterministic generated programs, compared across host/kernel/compiled/runIR
# and then through the standalone native compiler.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

cases=${RANDOM_CASES:-25}
base_seed=${RANDOM_SEED:-1729}
work=$(mktemp -d "${TMPDIR:-/tmp}/arturo-random.XXXXXX")
trap 'rm -rf "$work"' EXIT

pass=0
for ((index=0; index<cases; index++)); do
    seed=$((base_seed+index))
    source="$work/random_${seed}.art"
    binary="$work/random_${seed}"
    python3 tools/random_differential.py --seed "$seed" --output "$source"
    result=$(arturo --no-color src/one_test.art "$source" 2>&1)
    if ! grep -qx 'OK' <<<"$result"; then
        echo "FAIL random seed=$seed (four-engine parity)" >&2
        cat "$source" >&2
        printf '%s\n' "$result" >&2
        exit 1
    fi
    ./tmp/ncomp "$source" "$binary" >/dev/null
    arturo --no-color "$source" >"$work/host.out" 2>"$work/host.err"
    ARTURO_NO_COLOR=1 "$binary" >"$work/native.out" 2>"$work/native.err"
    if ! cmp -s "$work/host.out" "$work/native.out" || ! cmp -s "$work/host.err" "$work/native.err"; then
        echo "FAIL random seed=$seed (native parity)" >&2
        cat "$source" >&2
        diff -u "$work/host.out" "$work/native.out" >&2 || true
        diff -u "$work/host.err" "$work/native.err" >&2 || true
        exit 1
    fi
    pass=$((pass+1))
done

echo "== randomized differential: pass=$pass seed=$base_seed =="
