#!/bin/bash
# Differentially execute vendored, unmodified tests from Arturo's pinned
# upstream revision.  Each engine gets an isolated working directory and all
# externally visible effects are compared.
set -u
cd "$(dirname "$0")/.." || exit 1
root=$PWD
work=$(mktemp -d /tmp/arturo-upstream.XXXXXX)
trap 'rm -rf "$work"' EXIT

capture() {
    local run_dir=$1 stdout=$2 stderr=$3 status=$4
    shift 4
    (
        cd "$run_dir" || exit 125
        "$@"
    ) >"$stdout" 2>"$stderr"
    printf '%s\n' "$?" >"$status"
}

pass=0
fail=0
buildfail=0
for source in upstream/*/*.art; do
    suite=$(basename "$(dirname "$source")")
    name=$(basename "$source" .art)
    case_name="${suite}_${name}"
    case_dir="$work/$case_name"
    host_dir="$case_dir/host-fs"
    native_dir="$case_dir/native-fs"
    binary="$case_dir/program"
    mkdir -p "$host_dir" "$native_dir"

    if ! ./tmp/ncomp "$root/$source" "$binary" >"$case_dir/build.stdout" 2>"$case_dir/build.stderr"; then
        echo "BUILDFAIL $case_name"
        buildfail=$((buildfail+1))
        continue
    fi
    capture "$host_dir" "$case_dir/host.stdout" "$case_dir/host.stderr" "$case_dir/host.status" \
        arturo --no-color "$root/$source"
    capture "$native_dir" "$case_dir/native.stdout" "$case_dir/native.stderr" "$case_dir/native.status" \
        env ARTURO_NO_COLOR=1 "$binary"

    if cmp -s "$case_dir/host.stdout" "$case_dir/native.stdout" \
        && cmp -s "$case_dir/host.stderr" "$case_dir/native.stderr" \
        && cmp -s "$case_dir/host.status" "$case_dir/native.status" \
        && diff -ru "$host_dir" "$native_dir" >/dev/null 2>&1; then
        echo "PASS $case_name"
        pass=$((pass+1))
    else
        echo "FAIL $case_name"
        diff -u "$case_dir/host.stdout" "$case_dir/native.stdout" || true
        diff -u "$case_dir/host.stderr" "$case_dir/native.stderr" || true
        diff -u "$case_dir/host.status" "$case_dir/native.status" || true
        diff -ru "$host_dir" "$native_dir" || true
        fail=$((fail+1))
    fi
done

echo "== upstream parity: pass=$pass fail=$fail buildfail=$buildfail =="
test "$fail" -eq 0 && test "$buildfail" -eq 0
