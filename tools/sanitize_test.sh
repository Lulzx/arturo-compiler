#!/bin/bash
# Compile the generated programs and runtime with ASan + UBSan, then execute
# the entire parity corpus. Leak detection is kept separate: the current value
# arena intentionally has process-lifetime allocations and needs ownership work
# before LeakSanitizer can be a release gate.  On platforms where LSan works
# (Linux) a leak REPORT is produced but does not fail the run, so the ownership
# boundary stays visible without silently accepting an incomplete gate.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
work=$(mktemp -d /tmp/arturo-sanitize.XXXXXX)
trap 'rm -rf "$work"' EXIT

cc_bin=${CC:-cc}
flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
crypto_lib=(-lcrypto)
leak_report=0
if [ "$(uname -s)" = Darwin ]; then crypto_lib=(); else leak_report=1; fi
"$cc_bin" "${flags[@]}" -Iruntime -c runtime/runtime.c -o "$work/runtime.o"

sanitize_source() {
    local source=$1 base=$2 generated="$work/$2"
    ./tmp/ncomp "$root/$source" "$generated" >/dev/null
    "$cc_bin" "${flags[@]}" -Iruntime -c "$generated.c" -o "$generated.o"
    "$cc_bin" "${flags[@]}" "$generated.o" "$work/runtime.o" -lm ${crypto_lib[@]+"${crypto_lib[@]}"} -o "$generated.san"
    mkdir -p "$generated.fs"
    (
        cd "$generated.fs"
        ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            ARTURO_NO_COLOR=1 "$generated.san" >/dev/null
    )
    echo "SANPASS $base"
}

leak_summary() {
    # Run one representative program with LeakSanitizer on and report the
    # definitely-lost bytes. Informative only: the value arena deliberately
    # uses process-lifetime allocation until ownership is made explicit.
    local base=$1 generated="$work/$2"
    (
        cd "$generated.fs"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
            ARTURO_NO_COLOR=1 "$generated.san" >/dev/null 2>"$generated.lsan"
    ) || true
    local definitely=$(grep -m1 "Definitely lost:" "$generated.lsan" | awk '{print $3, $4, $5}')
    echo "LSAN $base: definitely lost ${definitely:-n/a}"
}

corpus_pass=0
for source in corpus/*.art; do
    base=$(basename "$source" .art)
    sanitize_source "$source" "$base"
    corpus_pass=$((corpus_pass+1))
done

upstream_pass=0
for source in upstream/*/*.art; do
    suite=$(basename "$(dirname "$source")")
    base="upstream_${suite}_$(basename "$source" .art)"
    sanitize_source "$source" "$base"
    upstream_pass=$((upstream_pass+1))
done

if [ "$leak_report" -eq 1 ]; then
    first=$(ls corpus/*.art | head -1)
    leak_summary "$(basename "$first" .art)" "$(basename "$first" .art)"
fi

echo "== sanitizer: corpus=$corpus_pass upstream=$upstream_pass =="
