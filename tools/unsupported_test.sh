#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

work=$(mktemp -d "${TMPDIR:-/tmp}/arturo-unsupported.XXXXXX")
trap 'rm -rf "$work"' EXIT

awk -F '\t' '!/^#/ && $3 == "unsupported" { print $1 }' config/intrinsic-policy.tsv | sort >"$work/policy"
sed -n '/^UNSUPPORTED_INTRINSICS:/,/^]/p' src/semantics.art \
    | grep -Eo '"[^"]+"' | tr -d '"' | sort -u >"$work/compiler"
if ! diff -u "$work/policy" "$work/compiler"; then
    echo "FAIL unsupported policy and compiler rejection table differ" >&2
    exit 1
fi

set +e
./tmp/ncomp fixtures/unsupported/browse.art "$work/program" >"$work/output" 2>&1
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "FAIL unsupported capability compiled successfully" >&2
    exit 1
fi
if ! grep -q 'unsupported standalone capability: browse' "$work/output"; then
    echo "FAIL missing stable unsupported-capability diagnostic" >&2
    cat "$work/output" >&2
    exit 1
fi
if [ -e "$work/program" ]; then
    echo "FAIL unsupported capability produced an executable" >&2
    exit 1
fi

echo "PASS unsupported capability rejection"
