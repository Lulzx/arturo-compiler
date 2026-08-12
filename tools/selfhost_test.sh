#!/bin/bash
# Self-hosting differential test: the self-compiled native compiler builds each
# corpus program.  Parity means stdout, stderr, exit status, and filesystem
# effects all match; keeping stderr merged into stdout used to hide ordering and
# status mismatches.
set -u
cd "$(dirname "$0")/.."
root=$PWD
work=$(mktemp -d /tmp/arturo-selfhost.XXXXXX)
trap 'rm -rf "$work"' EXIT

run_capture() {
    local run_dir=$1 stdout=$2 stderr=$3 status=$4
    shift 4
    (
        cd "$run_dir" || exit 125
        "$@"
    ) >"$stdout" 2>"$stderr"
    printf '%s\n' "$?" >"$status"
}

pass=0; fail=0; buildfail=0
for f in corpus/*.art; do
    base=$(basename "$f" .art)
    case_dir="$work/$base"
    host_dir="$case_dir/host-fs"
    native_dir="$case_dir/native-fs"
    mkdir -p "$host_dir" "$native_dir"
    out="$case_dir/program"

    if ! ./tmp/ncomp "$root/$f" "$out" >"$case_dir/build.stdout" 2>"$case_dir/build.stderr"; then
        echo "BUILDFAIL $base"
        buildfail=$((buildfail+1))
        continue
    fi
    if [ ! -x "$out" ]; then
        echo "BUILDFAIL $base"
        buildfail=$((buildfail+1))
        continue
    fi

    run_capture "$host_dir" "$case_dir/host.stdout" "$case_dir/host.stderr" "$case_dir/host.status" \
        arturo --no-color "$root/$f"
    run_capture "$native_dir" "$case_dir/native.stdout" "$case_dir/native.stderr" "$case_dir/native.status" \
        env ARTURO_NO_COLOR=1 "$out"

    if cmp -s "$case_dir/host.stdout" "$case_dir/native.stdout" \
        && cmp -s "$case_dir/host.stderr" "$case_dir/native.stderr" \
        && cmp -s "$case_dir/host.status" "$case_dir/native.status" \
        && diff -ru "$host_dir" "$native_dir" >/dev/null 2>&1; then
        pass=$((pass+1))
        echo "PASS $base"
    else
        fail=$((fail+1))
        echo "FAIL $base"
        diff -u "$case_dir/host.stdout" "$case_dir/native.stdout" || true
        diff -u "$case_dir/host.stderr" "$case_dir/native.stderr" || true
        diff -u "$case_dir/host.status" "$case_dir/native.status" || true
        diff -ru "$host_dir" "$native_dir" || true
    fi
done
echo "== selfnative==host: pass=$pass fail=$fail buildfail=$buildfail =="
test "$fail" -eq 0 -a "$buildfail" -eq 0
