#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

declared=$(mktemp)
native=$(mktemp)
trap 'rm -f "$declared" "$native"' EXIT

sed -n 's/^    "\([^"]*\)":.*/\1/p' src/intrinsics.art | sort -u >"$declared"
{
    # Donated primitives implemented by the standalone runtime.
    grep -Eo '\{"[^"]+",b_[A-Za-z0-9_]+' runtime/runtime.c \
        | sed 's/^{"//; s/",b_.*//'
    # Structural language declarations are compiler-owned rules, not runtime
    # builtin calls. They still implement declared language surface and must
    # not be reported as missing merely because they bypass BUILTINS.
    sed -n 's/^defRule "\([^"]*\)".*/\1/p' src/kernel.art
} | sort -u >"$native"

declared_count=$(wc -l <"$declared" | tr -d ' ')
native_count=$(wc -l <"$native" | tr -d ' ')
covered_count=$(comm -12 "$declared" "$native" | wc -l | tr -d ' ')
missing_count=$((declared_count-covered_count))

echo "declared intrinsics: $declared_count"
echo "native intrinsics:   $native_count"
echo "covered declarations: $covered_count"
echo "missing declarations: $missing_count"
echo
echo "Native coverage is $covered_count/$declared_count declarations."
echo
echo "First 25 missing declarations:"
comm -23 "$declared" "$native" | sed -n '1,25p'
