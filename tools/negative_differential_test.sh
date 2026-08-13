#!/bin/bash
# Compare the observable failure contract of representative invalid programs.
# Arturo and the native runtime deliberately format diagnostics differently, so
# parity here means: both reject, neither dies by signal, and output emitted
# before the failure is identical. Exact native diagnostics have a separate gate.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/arturo-negative.XXXXXX")
trap 'rm -rf "$work"' EXIT

cases=(
    diagnostics/01_division_by_zero.art
    diagnostics/03_invalid_get.art
    diagnostics/04_invalid_logical.art
    diagnostics/06_missing_key.art
    diagnostics/07_out_of_bounds.art
    diagnostics/10_wrong_map_argument.art
    diagnostics/11_custom_error_kind.art
    diagnostics/12_ensure_failure.art
    diagnostics/13_import_error.art
    diagnostics/15_quantity_division_by_zero.art
    diagnostics/16_quantity_division_by_zero_quantity.art
)

pass=0
for source in "${cases[@]}"; do
    base=$(basename "$source" .art)
    binary="$work/$base"
    ./tmp/ncomp "$source" "$binary" >/dev/null

    set +e
    arturo --no-color "$source" >"$work/$base.host.out" 2>"$work/$base.host.err"
    host_status=$?
    ARTURO_NO_COLOR=1 "$binary" >"$work/$base.native.out" 2>"$work/$base.native.err"
    native_status=$?
    set -e

    if [ "$host_status" -eq 0 ] || [ "$native_status" -eq 0 ] || \
       [ "$host_status" -ge 128 ] || [ "$native_status" -ge 128 ]; then
        echo "FAIL negative $base: host=$host_status native=$native_status" >&2
        exit 1
    fi
    if ! cmp -s "$work/$base.host.out" "$work/$base.native.out"; then
        echo "FAIL negative $base: output before failure differs" >&2
        diff -u "$work/$base.host.out" "$work/$base.native.out" >&2 || true
        exit 1
    fi
    if ! grep -q 'runtime error' "$work/$base.native.err"; then
        echo "FAIL negative $base: native diagnostic missing" >&2
        cat "$work/$base.native.err" >&2
        exit 1
    fi
    pass=$((pass+1))
done

echo "== negative differential: pass=$pass =="
