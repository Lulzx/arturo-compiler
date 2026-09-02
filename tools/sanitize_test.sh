#!/bin/bash
# Compile the generated programs and runtime with ASan + UBSan, then execute
# the entire parity corpus. Linux enables LeakSanitizer as a hard gate; macOS's
# Apple Clang ASan does not provide LSan, but still runs address/UB checks.
#
# Programs run in parallel (SANITIZE_JOBS, default: CPU count) and every stage
# has a timeout, so a single wedged program fails the run with its name instead
# of silently consuming the CI job's time budget.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
root=$PWD
work=$(mktemp -d /tmp/arturo-sanitize.XXXXXX)
trap 'rm -rf "$work"' EXIT

cc_bin=${CC:-cc}
flags=(-O1 -g -fno-omit-frame-pointer "-fsanitize=address,undefined")
crypto_lib=(-lcrypto)
detect_leaks=1
leak_label=enabled
if [ "$(uname -s)" = Darwin ]; then
    crypto_lib=()
    detect_leaks=0
    leak_label=unsupported
fi
jobs=${SANITIZE_JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)}
stage_timeout=${SANITIZE_STAGE_TIMEOUT:-600}

if command -v timeout >/dev/null 2>&1; then
    with_timeout() { timeout "$stage_timeout" "$@"; }
else
    with_timeout() { "$@"; }
fi

stamp() { date +%H:%M:%S; }

"$cc_bin" "${flags[@]}" -Iruntime -c runtime/runtime.c -o "$work/runtime.o"
echo "$(stamp) runtime compiled (jobs=$jobs stage_timeout=${stage_timeout}s)"

sanitize_source() {
    local source=$1 base=$2 generated="$work/$2" stage
    stage=compile
    if with_timeout ./tmp/ncomp "$root/$source" "$generated" >"$generated.ncomp.log" 2>&1 \
        && { stage=cc; with_timeout "$cc_bin" "${flags[@]}" -Iruntime -c "$generated.c" -o "$generated.o" 2>"$generated.cc.log"; } \
        && { stage=link; with_timeout "$cc_bin" "${flags[@]}" "$generated.o" "$work/runtime.o" -lm ${crypto_lib[@]+"${crypto_lib[@]}"} -o "$generated.san" 2>>"$generated.cc.log"; } \
        && { stage=run; mkdir -p "$generated.fs"; (
            cd "$generated.fs"
            ASAN_OPTIONS="detect_leaks=$detect_leaks:halt_on_error=1" \
                LSAN_OPTIONS=exitcode=23 \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                ARTURO_NO_COLOR=1 with_timeout "$generated.san" >/dev/null 2>"$generated.run.log"
        ); }
    then
        echo "$(stamp) SANPASS $base"
    else
        local status=$?
        echo "$(stamp) SANFAIL $base stage=$stage status=$status"
        for log in "$generated.ncomp.log" "$generated.cc.log" "$generated.run.log"; do
            [ -s "$log" ] && { echo "--- $(basename "$log")"; head -60 "$log"; }
        done
        return 1
    fi
}
export -f sanitize_source with_timeout stamp
export work root cc_bin detect_leaks stage_timeout
export flags_str="${flags[*]}" crypto_str="${crypto_lib[*]:-}"

# xargs runs each program in a fresh bash; rebuild the arrays there.
worker='
    read -r -a flags <<<"$flags_str"; crypto_lib=(); [ -n "$crypto_str" ] && read -r -a crypto_lib <<<"$crypto_str"
    sanitize_source "$1" "$2"'

list="$work/list.txt"
for source in corpus/*.art; do
    printf '%s\t%s\n' "$source" "$(basename "$source" .art)"
done >"$list"
for source in upstream/*/*.art; do
    printf '%s\t%s\n' "$source" "upstream_$(basename "$(dirname "$source")")_$(basename "$source" .art)"
done >>"$list"

corpus_total=$(grep -c '^corpus/' "$list")
upstream_total=$(grep -c '^upstream/' "$list")

results="$work/results.txt"
# Run the slowest (largest) sources first so the tail of the run is short.
sort -t$'\t' -k1,1 "$list" | while IFS=$'\t' read -r src base; do
    printf '%s\t%s\t%s\n' "$(wc -c <"$src")" "$src" "$base"
done | sort -rn | cut -f2,3 | tr '\t' '\n' \
    | xargs -P "$jobs" -n 2 bash -c "$worker" _ | tee "$results" || true

pass=$(grep -c ' SANPASS ' "$results" || true)
fail=$(grep -c ' SANFAIL ' "$results" || true)
expected=$((corpus_total + upstream_total))
echo "== sanitizer: corpus=$corpus_total upstream=$upstream_total pass=$pass fail=$fail leaks=$leak_label =="
if [ "$fail" -ne 0 ] || [ "$pass" -ne "$expected" ]; then
    echo "sanitizer: expected $expected passes, got $pass (fail=$fail)" >&2
    exit 1
fi
