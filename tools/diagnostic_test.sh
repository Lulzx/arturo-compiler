#!/bin/bash
# Stable uncaught-failure contract: diagnostics go to stderr, successful output
# remains on stdout, and every uncaught failure exits nonzero. Paths are
# normalized only so checked-in expectations are independent of checkout root.
set -u
cd "$(dirname "$0")/.." || exit 1
root=$PWD
work=$(mktemp -d /tmp/arturo-diagnostics.XXXXXX)
trap 'rm -rf "$work"' EXIT

failed=0
for source in diagnostics/*.art; do
    name=$(basename "$source" .art)
    binary="$work/$name"
    stdout="$work/$name.stdout"
    stderr="$work/$name.stderr"
    normalized="$work/$name.normalized.stderr"
    if ! ./tmp/ncomp "$root/$source" "$binary" >/dev/null 2>"$work/$name.build.stderr"; then
        echo "FAIL $name (build)"
        failed=$((failed+1))
        continue
    fi
    "$binary" >"$stdout" 2>"$stderr"
    status=$?
    sed "s|$root/$source|<source>|g" "$stderr" >"$normalized"
    if [ "$status" -ne 0 ] \
        && cmp -s "$stdout" "diagnostics/$name.stdout" \
        && cmp -s "$normalized" "diagnostics/$name.stderr"; then
        echo "PASS $name"
    else
        echo "FAIL $name (status=$status)"
        diff -u "diagnostics/$name.stdout" "$stdout" || true
        diff -u "diagnostics/$name.stderr" "$normalized" || true
        failed=$((failed+1))
    fi
done
test "$failed" -eq 0
