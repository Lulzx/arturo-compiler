#!/usr/bin/env bash
# Benchmark the native runtime against the host interpreter.
# For each tools/bench/*.art: compile with tmp/ncomp, run host and native
# (3 runs each, median wall time), verify identical stdout, print a table.
# BENCH_ONLY=name restricts to one program. Requires `make ncomp` first.
set -u
cd "$(dirname "$0")/.."
ARTURO=${ARTURO:-arturo}
RUNS=${BENCH_RUNS:-3}
OUT=tmp/bench; mkdir -p "$OUT"
case "$(uname -s)" in Darwin) TIMEFLAG=-l; RSSKEY="maximum resident set size";;
  *) TIMEFLAG=-v; RSSKEY="Maximum resident set size";; esac

now_ms(){ python3 -c 'import time;print(int(time.time()*1000))'; }
median(){ printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
timeit(){ # prints "median_ms rss_kb"; stdout of the program -> $2
  local -a ts=(); local rss=0
  for _ in $(seq "$RUNS"); do
    local t0; t0=$(now_ms)
    /usr/bin/time $TIMEFLAG "${@:3}" >"$2" 2>"$OUT/time.txt"
    local t1; t1=$(now_ms); ts+=($((t1-t0)))
    local r; r=$(grep "$RSSKEY" "$OUT/time.txt" | grep -oE '[0-9]+' | tail -1)
    [ -n "$r" ] && rss=$r
  done
  case "$TIMEFLAG" in -l) rss=$((rss/1024));; esac
  echo "$(median "${ts[@]}") $rss"
}

printf '%-16s %9s %9s %7s %9s  %s\n' program host_ms native_ms ratio native_MB parity
fail=0
for src in tools/bench/*.art; do
  name=$(basename "$src" .art)
  [ -n "${BENCH_ONLY:-}" ] && [ "$name" != "$BENCH_ONLY" ] && continue
  if ! ./tmp/ncomp "$src" "$OUT/$name" >"$OUT/$name.build" 2>&1; then
    printf '%-16s compile failed (see %s)\n' "$name" "$OUT/$name.build"; fail=1; continue
  fi
  read -r hms _ < <(timeit host "$OUT/$name.host" env ARTURO_NO_COLOR=1 "$ARTURO" --no-color "$src")
  read -r nms nrss < <(timeit native "$OUT/$name.native" env ARTURO_NO_COLOR=1 "$OUT/$name")
  if cmp -s "$OUT/$name.host" "$OUT/$name.native"; then par=ok; else par=MISMATCH; fail=1; fi
  ratio=$(awk -v h="$hms" -v n="$nms" 'BEGIN{ if(n==0) n=1; printf "%.2fx", h/n }')
  printf '%-16s %9s %9s %7s %9s  %s\n' "$name" "$hms" "$nms" "$ratio" "$((nrss/1024))" "$par"
done
exit $fail
