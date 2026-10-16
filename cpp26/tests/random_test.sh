#!/bin/bash
# Deterministic generated programs (the original compiler's generator in
# ../tools) compared host vs arturoc, so the C++ compiler is held to the
# same randomized differential evidence as the self-hosting one.
#
#   RANDOM_CASES=100 RANDOM_SEED=1729 bash tests/random_test.sh
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
ARTURO=${ARTURO:-arturo}
command -v "$ARTURO" >/dev/null 2>&1 || ARTURO="$ROOT/arturo-bin/arturo"
if ! command -v "$ARTURO" >/dev/null 2>&1 && [ ! -x "$ARTURO" ]; then
    echo "error: pinned arturo host not found (set ARTURO=...)" >&2
    exit 1
fi
WORK=$(mktemp -d /tmp/arturoc-random.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
cases=${RANDOM_CASES:-100}
base_seed=${RANDOM_SEED:-1729}
pass=0; fail=0
for ((index=0; index<cases; index++)); do
    seed=$((base_seed+index))
    src="$WORK/random_${seed}.art"
    python3 "$ROOT/tools/random_differential.py" --seed "$seed" --output "$src"
    (cd "$WORK" && "$ARTURO" --no-color "$src" >host.out 2>host.err); hs=$?
    for backend in fast direct; do
        if [ "$backend" = fast ]; then ./bin/arturoc "$src" "$WORK/p" >"$WORK/c.log" 2>&1
        else ./bin/arturoc --backend=$backend "$src" "$WORK/p" >"$WORK/c.log" 2>&1; fi
        if [ $? -ne 0 ]; then echo "FAIL seed=$seed ($backend) compile: $(cat "$WORK/c.log")"; fail=$((fail+1)); continue; fi
        (cd "$WORK" && ./p >new.out 2>new.err); ns=$?
        # Error text differs by design (CONTRACT.md SS7); stdout and the
        # success/failure status must agree with the host.
        if cmp -s "$WORK/host.out" "$WORK/new.out" && [ "$hs" = "$ns" ]; then pass=$((pass+1))
        else echo "FAIL seed=$seed ($backend) parity host=$hs cpp=$ns"; fail=$((fail+1)); fi
    done
done
echo "== arturoc randomized differential: pass=$pass fail=$fail seeds=$base_seed..$((base_seed+cases-1)) =="
[ "$fail" -eq 0 ]
