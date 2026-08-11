#!/bin/bash
# Expected-output tests for deliberate fixes to documented Arturo 0.10.0 bugs.
set -u
cd "$(dirname "$0")/.."

out=/tmp/arturo_compiler_compat_append
rm -f "$out"
if ! ./tmp/ncomp compat/01_string_integer_append.art "$out" >/dev/null 2>&1; then
    echo "FAIL string_integer_append (build)"
    exit 1
fi
if diff <("$out" 2>&1) compat/01_string_integer_append.out >/dev/null 2>&1; then
    echo "PASS string_integer_append"
    exit 0
fi
echo "FAIL string_integer_append (output)"
exit 1
