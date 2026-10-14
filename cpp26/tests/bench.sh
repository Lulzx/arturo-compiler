#!/bin/bash
# Benchmarks: compilation latency (fast vs C vs direct vs the original ncomp)
# and run time (host vs fast vs direct) with output parity checked first.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(cd .. && pwd)
ARTURO=${ARTURO:-arturo}
command -v "$ARTURO" >/dev/null 2>&1 || ARTURO="$ROOT/arturo-bin/arturo"
WORK=$(mktemp -d /tmp/arturoc-bench.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

median_ms() { # median_ms <reps> <cmd...>
    local reps=$1; shift
    python3 - "$reps" "$@" <<'PY'
import subprocess, sys, time
reps = int(sys.argv[1]); cmd = sys.argv[2:]
ts = []
for _ in range(reps):
    t0 = time.perf_counter()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ts.append((time.perf_counter() - t0) * 1000)
ts.sort()
print(f"{ts[len(ts)//2]:.1f}")
PY
}

echo "== compilation latency (median of 5) =="
printf '%-28s %10s %10s %10s %10s\n' program fast_ms c_ms direct_ms ncomp_ms
for src in "$ROOT/corpus/05_functions.art" "$ROOT/corpus/50_defined_types.art" "$ROOT/src/compiler.art"; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    f=$(median_ms 5 ./bin/arturoc "$src" "$WORK/out-fast")
    c=$(median_ms 5 ./bin/arturoc --backend=c "$src" "$WORK/out-c")
    d=$(median_ms 5 ./bin/arturoc --backend=direct "$src" "$WORK/out-direct")
    if [ -x "$ROOT/tmp/ncomp" ]; then
        n=$(median_ms 5 "$ROOT/tmp/ncomp" "$src" "$WORK/out-ncomp")
    else n="n/a"; fi
    printf '%-28s %10s %10s %10s %10s\n' "$name" "$f" "$c" "$d" "$n"
done

echo "== run time (median of 5, parity checked) =="
printf '%-28s %10s %10s %10s %8s\n' program host_ms fast_ms direct_ms parity
for src in "$ROOT"/tools/bench/*.art; do
    name=$(basename "$src" .art)
    ./bin/arturoc "$src" "$WORK/run-fast" >/dev/null 2>&1 || { printf '%-28s compile failed (fast)\n' "$name"; continue; }
    ./bin/arturoc --backend=direct "$src" "$WORK/run-direct" >/dev/null 2>&1 || { printf '%-28s compile failed (direct)\n' "$name"; continue; }
    (cd "$WORK" && "$ARTURO" --no-color "$src" > host.out 2>/dev/null)
    (cd "$WORK" && ./run-fast > fast.out 2>/dev/null)
    (cd "$WORK" && ./run-direct > direct.out 2>/dev/null)
    par=ok
    cmp -s "$WORK/host.out" "$WORK/fast.out" || par=FAST-MISMATCH
    cmp -s "$WORK/host.out" "$WORK/direct.out" || par=DIRECT-MISMATCH
    h=$(median_ms 5 "$ARTURO" --no-color "$src")
    f=$(median_ms 5 "$WORK/run-fast")
    d=$(median_ms 5 "$WORK/run-direct")
    printf '%-28s %10s %10s %10s %8s\n' "$name" "$h" "$f" "$d" "$par"
done
