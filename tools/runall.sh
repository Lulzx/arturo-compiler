#!/bin/bash
# build each corpus program natively and diff its stdout against the host
cd "$(dirname "$0")/.."
pass=0; fail=0; skip=0
for f in corpus/*.art; do
    base=$(basename "$f" .art)
    arturo tools/cbuild.art "$f" "tmp/$base" >/dev/null 2>&1
    if [ ! -x "tmp/$base" ]; then echo "BUILDFAIL $base"; skip=$((skip+1)); continue; fi
    if diff <(./tmp/$base) <(arturo --no-color "$f") >/dev/null 2>&1; then
        pass=$((pass+1)); echo "PASS $base"
    else
        fail=$((fail+1)); echo "FAIL $base"
    fi
done
echo "== pass=$pass fail=$fail buildfail=$skip =="
