#!/bin/bash
# Expected-output tests for deliberate fixes to documented Arturo 0.10.0 bugs.
set -u
cd "$(dirname "$0")/.." || exit 1

failed=0
for case_name in 01_string_integer_append 02_unescape 03_module_values 04_custom_units 05_quantity_dimensions; do
    out="/tmp/arturo_compiler_compat_${case_name}"
    rm -f "$out"
    if ! ./tmp/ncomp "compat/${case_name}.art" "$out" >/dev/null 2>&1; then
        echo "FAIL $case_name (build)"
        failed=$((failed+1))
        continue
    fi
    if diff <("$out" 2>&1) "compat/${case_name}.out" >/dev/null 2>&1; then
        echo "PASS $case_name"
    else
        echo "FAIL $case_name (output)"
        diff -u "compat/${case_name}.out" <("$out" 2>&1) || true
        failed=$((failed+1))
    fi
done
test "$failed" -eq 0
