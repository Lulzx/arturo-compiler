#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

declared=$(mktemp)
native=$(mktemp)
missing=$(mktemp)
classified=$(mktemp)
trap 'rm -f "$declared" "$native" "$missing" "$classified"' EXIT

sed -n 's/^    "\([^"]*\)":.*/\1/p' src/intrinsics.art | sort -u >"$declared"
{
    # Donated primitives implemented by the standalone runtime.
    grep -Eo '\{"[^"]+",b_[A-Za-z0-9_]+' runtime/runtime.c \
        | sed 's/^{"//; s/",b_.*//'
    # Structural language declarations are compiler-owned rules, not runtime
    # builtin calls. They still implement declared language surface and must
    # not be reported as missing merely because they bypass BUILTINS.
    sed -n 's/^defRule "\([^"]*\)".*/\1/p' src/kernel.art
    # Compile-time dependency forms disappear before IR emission.
    sed '/^#/d; /^[[:space:]]*$/d' config/compiler-owned-intrinsics.txt
} | sort -u >"$native"
comm -23 "$declared" "$native" >"$missing"

# Every missing declaration must have an explicit product decision.  Keeping
# this machine-checked prevents the compatibility boundary from turning back
# into an informal backlog when the pinned Arturo revision changes.
awk -F '\t' '!/^#/ && NF { print $1 }' config/intrinsic-policy.tsv | sort -u >"$classified"
unclassified=$(comm -23 "$missing" "$classified")
stale=$(comm -13 "$missing" "$classified")
if [ -n "$unclassified" ] || [ -n "$stale" ]; then
    echo "intrinsic policy is out of sync" >&2
    if [ -n "$unclassified" ]; then
        echo "unclassified declarations:" >&2
        echo "$unclassified" >&2
    fi
    if [ -n "$stale" ]; then
        echo "classified but already implemented/not declared:" >&2
        echo "$stale" >&2
    fi
    exit 1
fi

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
echo "Missing declaration policy:"
awk -F '\t' '!/^#/ && NF { counts[$2]++ } END {
    for (k in counts) print "  " k ": " counts[k]
}' config/intrinsic-policy.tsv | sort
required_count=$(awk -F '\t' '!/^#/ && $3 == "required" { n++ } END { print n+0 }' config/intrinsic-policy.tsv)
unsupported_count=$(awk -F '\t' '!/^#/ && $3 == "unsupported" { n++ } END { print n+0 }' config/intrinsic-policy.tsv)
echo
echo "Remaining required declarations: $required_count"
echo "Intentionally unavailable declarations: $unsupported_count"
echo
# The compatibility boundary is complete only when every missing declaration
# has an explicit unsupported decision and nothing classified as unavailable
# has drifted back into the implemented set.
if [ "$missing_count" -ne "$unsupported_count" ]; then
    echo "classification is NOT complete: missing != intentionally unavailable" >&2
    exit 1
fi
echo "Classification: $declared_count/$declared_count declared intrinsics have an explicit product decision."
echo
echo "First 25 missing declarations:"
sed -n '1,25p' "$missing"
