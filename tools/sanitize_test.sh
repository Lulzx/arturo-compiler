#!/bin/bash
# Compile the generated programs and runtime with ASan + UBSan, then execute
# the entire parity corpus. Leak detection is kept separate: the current value
# arena intentionally has process-lifetime allocations and needs ownership work
# before LeakSanitizer can be a release gate.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
work=$(mktemp -d /tmp/arturo-sanitize.XXXXXX)
trap 'rm -rf "$work"' EXIT

cc_bin=${CC:-cc}
flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
crypto_lib=(-lcrypto)
if [ "$(uname -s)" = Darwin ]; then crypto_lib=(); fi
"$cc_bin" "${flags[@]}" -Iruntime -c runtime/runtime.c -o "$work/runtime.o"

sanitize_source() {
    local source=$1 base=$2 generated="$work/$2"
    ./tmp/ncomp "$root/$source" "$generated" >/dev/null
    "$cc_bin" "${flags[@]}" -Iruntime -c "$generated.c" -o "$generated.o"
    "$cc_bin" "${flags[@]}" "$generated.o" "$work/runtime.o" -lm "${crypto_lib[@]}" -o "$generated.san"
    mkdir -p "$generated.fs"
    (
        cd "$generated.fs"
        ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            ARTURO_NO_COLOR=1 "$generated.san" >/dev/null
    )
    echo "SANPASS $base"
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

echo "== sanitizer: corpus=$corpus_pass upstream=$upstream_pass =="
