#!/bin/bash
# self-hosting differential test: the self-compiled native compiler (tmp/ncomp)
# builds each corpus program; its stdout must match the host interpreter.
cd "$(dirname "$0")/.."
pass=0; fail=0; buildfail=0
for f in corpus/*.art; do
    base=$(basename "$f" .art)
    out="/tmp/sel_$base"
    rm -f "$out"
    if ! ./tmp/ncomp "$f" "$out" >/dev/null 2>&1; then
        echo "BUILDFAIL $base"; buildfail=$((buildfail+1)); continue
    fi
    if [ ! -x "$out" ]; then
        echo "BUILDFAIL $base"; buildfail=$((buildfail+1)); continue
    fi
    if diff <("$out" 2>&1) <(arturo --no-color "$f" 2>&1) >/dev/null 2>&1; then
        pass=$((pass+1)); echo "PASS $base"
    else
        fail=$((fail+1)); echo "FAIL $base"
    fi
done
echo "== selfnative==host: pass=$pass fail=$fail buildfail=$buildfail =="
test "$fail" -eq 0 -a "$buildfail" -eq 0
