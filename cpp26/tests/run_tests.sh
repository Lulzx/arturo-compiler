#!/bin/bash
# Differential test suite for arturoc (C++26) against the pinned Arturo host
# and, where available, the original self-hosting compiler's native output.
#
#   1. fast backend == host, byte for byte (owned corpus + owned upstream)
#   2. optimizer equivalence: -O1 output == -O0 output (owned corpus)
#   3. C and direct backends == host (representative subset)
#   4. unsupported capabilities are rejected at compile time, no executable
#   5. invalid programs fail nonzero in both engines
#   6. AIR emission is deterministic
#   7. regression programs for past optimizer/constant-pool bugs
#   8. malformed AIR payloads are rejected by the stub without a signal
#   9. CLI contract, C-backend shell quoting/cleanup, nesting-depth limits
set -u
cd "$(dirname "$0")/.." || exit 1
CPP=$PWD
ROOT=$(cd .. && pwd)
ARTURO=${ARTURO:-arturo}
command -v "$ARTURO" >/dev/null 2>&1 || ARTURO="$ROOT/arturo-bin/arturo"
if ! command -v "$ARTURO" >/dev/null 2>&1 && [ ! -x "$ARTURO" ]; then
    echo "error: pinned arturo host not found (set ARTURO=...)" >&2
    exit 1
fi
WORK=$(mktemp -d /tmp/arturoc-tests.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
ok() { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $*"; }

run3() { # run3 <file> <bin> : compare host vs bin on stdout/stderr/status
    local src=$1 bin=$2
    (cd "$WORK" && "$ARTURO" --no-color "$src" >host.out 2>host.err); local hs=$?
    (cd "$WORK" && env ARTURO_NO_COLOR=1 "$bin" >new.out 2>new.err); local ns=$?
    cmp -s "$WORK/host.out" "$WORK/new.out" && cmp -s "$WORK/host.err" "$WORK/new.err" && [ "$hs" = "$ns" ]
}

echo "== fast backend vs host: owned corpus =="
while read -r name; do
    [ -n "$name" ] || continue
    src="$ROOT/corpus/$name.art"
    if ! ./bin/arturoc "$src" "$WORK/p" >"$WORK/c.log" 2>&1; then bad "compile $name: $(cat "$WORK/c.log")"; continue; fi
    if run3 "$src" "$WORK/p"; then ok; else bad "parity $name"; fi
done < tests/owned-corpus.txt

echo "== optimizer equivalence (-O0 vs -O1) =="
while read -r name; do
    [ -n "$name" ] || continue
    src="$ROOT/corpus/$name.art"
    ./bin/arturoc -O0 "$src" "$WORK/p0" >/dev/null 2>&1 || { bad "compile -O0 $name"; continue; }
    ./bin/arturoc -O1 "$src" "$WORK/p1" >/dev/null 2>&1 || { bad "compile -O1 $name"; continue; }
    (cd "$WORK" && ./p0 >o0.out 2>o0.err); s0=$?
    (cd "$WORK" && ./p1 >o1.out 2>o1.err); s1=$?
    if cmp -s "$WORK/o0.out" "$WORK/o1.out" && cmp -s "$WORK/o0.err" "$WORK/o1.err" && [ "$s0" = "$s1" ]; then ok; else bad "optimizer divergence $name"; fi
done < tests/owned-corpus.txt

echo "== fast backend vs host: owned upstream =="
while read -r rel; do
    [ -n "$rel" ] || continue
    src="$ROOT/$rel"
    if ! ./bin/arturoc "$src" "$WORK/p" >"$WORK/c.log" 2>&1; then bad "compile $rel: $(cat "$WORK/c.log")"; continue; fi
    if run3 "$src" "$WORK/p"; then ok; else bad "upstream parity $rel"; fi
done < tests/owned-upstream.txt

echo "== C and direct backends vs host (subset) =="
while read -r name; do
    [ -n "$name" ] || continue
    src="$ROOT/corpus/$name.art"
    for backend in c direct; do
        if ! ./bin/arturoc --backend=$backend "$src" "$WORK/p" >"$WORK/c.log" 2>&1; then
            bad "compile ($backend) $name: $(cat "$WORK/c.log")"; continue
        fi
        if run3 "$src" "$WORK/p"; then ok; else bad "parity ($backend) $name"; fi
    done
done < tests/backend-subset.txt

echo "== compat exceptions match the original native compiler =="
# The documented host defects (compat/ in the parent repo) keep the
# original native semantics here too -- most visibly string ++ integer,
# which the host rejects but both native compilers stringify. Golden
# outputs are the original's own compat/*.out files.
for src in "$ROOT"/compat/*.art; do
    name=$(basename "$src" .art)
    if ! ./bin/arturoc "$src" "$WORK/p" >"$WORK/c.log" 2>&1; then bad "compile compat $name: $(cat "$WORK/c.log")"; continue; fi
    (cd "$WORK" && ./p > compat.out 2>compat.err)
    if cmp -s "$WORK/compat.out" "$ROOT/compat/$name.out"; then ok; else bad "compat $name"; fi
done

echo "== unsupported capabilities rejected at compile time =="
printf 'print eval "1 + 2"\n' > "$WORK/unsupported.art"
if ./bin/arturoc "$WORK/unsupported.art" "$WORK/should-not-exist" >"$WORK/c.log" 2>&1; then
    bad "unsupported capability compiled successfully"
elif [ -e "$WORK/should-not-exist" ]; then
    bad "unsupported capability produced an executable"
elif grep -q "unsupported standalone capability" "$WORK/c.log"; then ok
else bad "unsupported capability gave an unclear diagnostic: $(cat "$WORK/c.log")"; fi

echo "== invalid programs fail in both engines =="
printf 'print definitelyUnboundName123\n' > "$WORK/invalid.art"
"$ARTURO" --no-color "$WORK/invalid.art" >/dev/null 2>&1; hs=$?
./bin/arturoc "$WORK/invalid.art" "$WORK/invalid-bin" >/dev/null 2>&1 || { bad "invalid program did not compile"; hs=0; }
if [ "${hs:-0}" != "0" ]; then
    (cd "$WORK" && ./invalid-bin >/dev/null 2>&1); ns=$?
    if [ "$ns" != "0" ] && [ "$ns" -lt 128 ]; then ok; else bad "invalid program exit status $ns"; fi
fi

echo "== AIR determinism =="
./bin/arturoc "$ROOT/corpus/05_functions.art" "$WORK/d0" --emit-ir "$WORK/d0.air" >/dev/null 2>&1
./bin/arturoc "$ROOT/corpus/05_functions.art" "$WORK/d1" --emit-ir "$WORK/d1.air" >/dev/null 2>&1
if cmp -s "$WORK/d0.air" "$WORK/d1.air"; then ok; else bad "AIR emission is not deterministic"; fi

echo "== regression: optimizer and constant-pool correctness =="
# Each program pins a bug found by differential probing: float constants
# closer than 1e-6 used to merge in the constant pool, integer comparisons
# above 2^53 were folded through double, and constant propagation survived
# statements (if/try/do) whose bodies rebind the propagated name.
cat > "$WORK/reg_floats.art" <<'EOF'
print 1.0000001
print 1.0000002
print 0.00000001
print 0.00000002
print 9007199254740993 = 9007199254740992
print 9007199254740993 > 9007199254740992
print 9223372036854775807 = 9223372036854775806
print 1000000 ^ 20
print 3 ^ 40
EOF
cat > "$WORK/reg_propagate.art" <<'EOF'
b: true
print do [
    x: 1
    if b [ x: 5 ]
    range x 7
]
print do [
    x: 1
    try [ x: 5 ]
    range x 7
]
print do [
    x: 1
    do [ x: 5 ]
    range x 7
]
print do [
    x: 1
    x: 5
    range x 7
]
EOF
cat > "$WORK/reg_branch.art" <<'EOF'
if false [ print "never" ]
unless true [ print "never2" ]
print "done"
EOF
cat > "$WORK/reg_slice.art" <<'EOF'
lo: 2
hi: 0
print slice "abcdefgh" lo hi
print slice "abcdefgh" 3 1
print slice "hello" 0 99
print slice "hello" 3 99
print slice [1 2 3 4 5] 0 99
print slice [1 2 3 4 5] 3 99
print size slice [1 2 3 4 5] 1 0
neg: 0-2
print slice [1 2 3 4 5] neg 2
print slice "hello" neg 2
a: "hello"
slice 'a 0 99
print a
b: [1 2 3 4 5]
slice 'b 0 99
print b
slice 'b 1 3
print b
EOF
cat > "$WORK/reg_predicates.art" <<'EOF'
print standalone?
print superuser?
print bytecode? "x"
print bytecode? [1 2]
print database? "x"
print database? 123
print socket? null
print store? #[]
print permissions "reg_predicates.art"
print permissions "definitely-not-here-12345"
print get terminal 'width
print type terminal
EOF
for reg in reg_floats reg_propagate reg_branch reg_slice reg_predicates; do
    src="$WORK/$reg.art"
    if ! ./bin/arturoc "$src" "$WORK/p" >"$WORK/c.log" 2>&1; then bad "compile $reg: $(cat "$WORK/c.log")"; continue; fi
    if run3 "$src" "$WORK/p"; then ok; else bad "parity $reg"; fi
    ./bin/arturoc -O0 "$src" "$WORK/p0" >/dev/null 2>&1 || { bad "compile -O0 $reg"; continue; }
    ./bin/arturoc -O1 "$src" "$WORK/p1" >/dev/null 2>&1 || { bad "compile -O1 $reg"; continue; }
    (cd "$WORK" && ./p0 >o0.out 2>o0.err); s0=$?
    (cd "$WORK" && ./p1 >o1.out 2>o1.err); s1=$?
    if cmp -s "$WORK/o0.out" "$WORK/o1.out" && cmp -s "$WORK/o0.err" "$WORK/o1.err" && [ "$s0" = "$s1" ]; then ok; else bad "optimizer divergence $reg"; fi
done
cat > "$WORK/reg_direct.art" <<'EOF'
print [[1 2] [3 4]]
print [1 [2 [3]]]
y: 1
g: function [] .export:[y][
    y: y + 10
]
g
print y
EOF
# Nested block constants need the C emitter's block_of helper in the direct
# backend's preamble too, and an attributed (.export) function must not be
# lowered as a plain binding-stable C function.
for reg in reg_floats reg_direct; do
    if ! ./bin/arturoc "$WORK/$reg.art" "$WORK/p" >"$WORK/c.log" 2>&1; then bad "compile $reg: $(cat "$WORK/c.log")"; continue; fi
    if run3 "$WORK/$reg.art" "$WORK/p"; then ok; else bad "parity $reg"; fi
done
for backend in c direct; do
    for reg in reg_floats reg_direct; do
        if ! ./bin/arturoc --backend=$backend "$WORK/$reg.art" "$WORK/p" >"$WORK/c.log" 2>&1; then
            bad "compile ($backend) $reg: $(cat "$WORK/c.log")"; continue
        fi
        if run3 "$WORK/$reg.art" "$WORK/p"; then ok; else bad "parity ($backend) $reg"; fi
    done
    for rel in upstream/unittests/scopes.art upstream/unittests/paths.art; do
        if ! ./bin/arturoc --backend=$backend "$ROOT/$rel" "$WORK/p" >"$WORK/c.log" 2>&1; then
            bad "compile ($backend) $rel: $(cat "$WORK/c.log")"; continue
        fi
        if run3 "$ROOT/$rel" "$WORK/p"; then ok; else bad "parity ($backend) $rel"; fi
    done
done

# A reversed block slice whose gap exceeds one index is a runtime error in
# the host (it attempts the Nim seq slice); both engines must fail nonzero
# without a signal, with identical stdout up to the failure. Error *text*
# intentionally differs (see CONTRACT.md SS7), so only stdout/status compare.
printf 'print "before"\nprint slice [1 2 3 4 5] 3 0\nprint "after"\n' > "$WORK/reg_slice_err.art"
if ./bin/arturoc "$WORK/reg_slice_err.art" "$WORK/p" >"$WORK/c.log" 2>&1; then
    (cd "$WORK" && "$ARTURO" --no-color reg_slice_err.art >host.out 2>/dev/null); hs=$?
    (cd "$WORK" && ./p >new.out 2>/dev/null); ns=$?
    if [ "$hs" != "0" ] && [ "$ns" != "0" ] && [ "$ns" -lt 128 ] && cmp -s "$WORK/host.out" "$WORK/new.out"; then ok; else bad "slice reversed-block error (host=$hs cpp=$ns)"; fi
else bad "compile reg_slice_err: $(cat "$WORK/c.log")"; fi

# `future?` type-errors on every value a compiled program can hold (no
# future can be constructed); host and compiled must both fail nonzero
# without a signal. Error text differs, so only the status is compared.
printf 'print future? 123\n' > "$WORK/reg_future.art"
if ./bin/arturoc "$WORK/reg_future.art" "$WORK/p" >"$WORK/c.log" 2>&1; then
    (cd "$WORK" && ./p >/dev/null 2>&1); ns=$?
    if [ "$ns" != "0" ] && [ "$ns" -lt 128 ]; then ok; else bad "future? on non-future (status $ns)"; fi
else bad "compile reg_future: $(cat "$WORK/c.log")"; fi

echo "== runtime errors carry the source line (all backends) =="
cat > "$WORK/errline.art" <<'EOF'
print "start"
x: [1 2 3]
f: function [v][
    print slice v 3 0
]
f x
EOF
for backend in fast c direct; do
    if [ "$backend" = fast ]; then ./bin/arturoc "$WORK/errline.art" "$WORK/p" >"$WORK/c.log" 2>&1
    else ./bin/arturoc --backend=$backend "$WORK/errline.art" "$WORK/p" >"$WORK/c.log" 2>&1; fi
    if [ $? -ne 0 ]; then bad "compile errline ($backend): $(cat "$WORK/c.log")"; continue; fi
    (cd "$WORK" && ./p >e.out 2>e.err); ns=$?
    if [ "$ns" != "0" ] && [ "$ns" -lt 128 ] && grep -q "at .*/errline.art:4" "$WORK/e.err"; then ok; else bad "error line ($backend, status $ns): $(cat "$WORK/e.err")"; fi
done

echo "== recompiling the same output path produces runnable binaries =="
# Regression: fast-backend outputs are replaced atomically; rapidly
# re-creating one path must never yield a binary the OS kills on launch
# (observed on macOS with in-place truncation + code-signature caching).
printf 'print "rerun-ok"\n' > "$WORK/rerun.art"
rerun_fail=0
for i in $(seq 1 25); do
    ./bin/arturoc "$WORK/rerun.art" "$WORK/rerun-bin" >/dev/null 2>&1 || { rerun_fail=1; break; }
    out=$("$WORK/rerun-bin" 2>/dev/null) || { rerun_fail=1; break; }
    [ "$out" = "rerun-ok" ] || { rerun_fail=1; break; }
done
if [ "$rerun_fail" -eq 0 ]; then ok; else bad "recompiled output not runnable at iteration $i"; fi

echo "== zip/unzip/volume parity (isolated directories, tree compared) =="
cat > "$WORK/zipprog.art" <<'EOF'
write "alpha content 123\n" "a.txt"
write "beta beta beta\n" "b.txt"
zip "out.zip" ["a.txt" "b.txt" "missing.txt"]
unzip "ext" "out.zip"
print read "ext/a.txt"
print read "ext/b.txt"
print volume "a.txt"
unzip "nowhere" "missing.zip"
print "done"
EOF
mkdir -p "$WORK/zip-host" "$WORK/zip-cpp"
if ./bin/arturoc "$WORK/zipprog.art" "$WORK/zipbin" >"$WORK/c.log" 2>&1; then
    cp "$WORK/zipprog.art" "$WORK/zip-host/p.art"; cp "$WORK/zipprog.art" "$WORK/zip-cpp/p.art"
    (cd "$WORK/zip-host" && "$ARTURO" --no-color p.art >out.txt 2>err.txt); hs=$?
    (cd "$WORK/zip-cpp" && "$WORK/zipbin" >out.txt 2>err.txt); ns=$?
    zipok=1
    cmp -s "$WORK/zip-host/out.txt" "$WORK/zip-cpp/out.txt" || zipok=0
    cmp -s "$WORK/zip-host/ext/a.txt" "$WORK/zip-cpp/ext/a.txt" || zipok=0
    cmp -s "$WORK/zip-host/ext/b.txt" "$WORK/zip-cpp/ext/b.txt" || zipok=0
    [ "$hs" = "$ns" ] || zipok=0
    # cross-extraction: each engine must read the other's archive
    mkdir -p "$WORK/zip-cross"
    printf 'unzip "ext" "out.zip"\nprint read "ext/b.txt"\n' > "$WORK/zip-cross/x.art"
    cp "$WORK/zip-host/out.zip" "$WORK/zip-cross/out.zip"
    ./bin/arturoc "$WORK/zip-cross/x.art" "$WORK/zipxbin" >/dev/null 2>&1 || zipok=0
    (cd "$WORK/zip-cross" && "$WORK/zipxbin" >x.out 2>/dev/null) || zipok=0
    grep -q "beta" "$WORK/zip-cross/x.out" || zipok=0
    if [ "$zipok" -eq 1 ]; then ok; else bad "zip/unzip/volume parity"; fi
else bad "compile zipprog: $(cat "$WORK/c.log")"; fi

echo "== cursor/goto escape bytes and browse opener (host vs compiled) =="
printf 'cursor false\ncursor true\ngoto 10 15\ngoto 5 null\ngoto null 3\nprint "x"\n' > "$WORK/cg.art"
if ./bin/arturoc "$WORK/cg.art" "$WORK/cgbin" >"$WORK/c.log" 2>&1; then
    (cd "$WORK" && "$ARTURO" --no-color cg.art >cg.host 2>/dev/null)
    (cd "$WORK" && ./cgbin >cg.cpp 2>/dev/null)
    if cmp -s "$WORK/cg.host" "$WORK/cg.cpp"; then ok; else bad "cursor/goto bytes"; fi
else bad "compile cg: $(cat "$WORK/c.log")"; fi
mkdir -p "$WORK/fakebin"
printf '#!/bin/sh\necho "OPENER: $@" >> "%s/fakebin/log"\n' "$WORK" > "$WORK/fakebin/open"
printf '#!/bin/sh\necho "OPENER: $@" >> "%s/fakebin/log"\n' "$WORK" > "$WORK/fakebin/xdg-open"
chmod +x "$WORK/fakebin/open" "$WORK/fakebin/xdg-open"
printf 'browse "https://example.com/page"\nprint "after"\n' > "$WORK/browse.art"
if ./bin/arturoc "$WORK/browse.art" "$WORK/browsebin" >"$WORK/c.log" 2>&1; then
    # the opener is launched without waiting; give its log a moment to land
    wait_log() { local i; for i in $(seq 1 40); do [ -s "$WORK/fakebin/log" ] && break; sleep 0.05; done; cat "$WORK/fakebin/log" 2>/dev/null; }
    rm -f "$WORK/fakebin/log"
    (cd "$WORK" && PATH="$WORK/fakebin:$PATH" "$ARTURO" --no-color browse.art >b.host 2>/dev/null)
    hostlog=$(wait_log); rm -f "$WORK/fakebin/log"
    (cd "$WORK" && PATH="$WORK/fakebin:$PATH" ./browsebin >b.cpp 2>/dev/null)
    cpplog=$(wait_log)
    if [ "$hostlog" = "$cpplog" ] && [ -n "$hostlog" ] && cmp -s "$WORK/b.host" "$WORK/b.cpp"; then ok; else bad "browse opener (host='$hostlog' cpp='$cpplog')"; fi
else bad "compile browse: $(cat "$WORK/c.log")"; fi

echo "== store parity: cross-engine persistence (host vs compiled) =="
if ARTURO_HOST="$ARTURO" python3 tests/store_test.py "$WORK" >"$WORK/store.log" 2>&1; then ok; else bad "store parity: $(cat "$WORK/store.log")"; fi

echo "== HTTP parity: request/download vs local server (host vs compiled) =="
if ARTURO_HOST="$ARTURO" python3 tests/http_test.py "$WORK" >"$WORK/http.log" 2>&1; then ok; else bad "http parity: $(cat "$WORK/http.log")"; fi

echo "== socket parity over live TCP (host vs compiled) =="
if ARTURO_HOST="$ARTURO" python3 tests/socket_test.py "$WORK" >"$WORK/sock.log" 2>&1; then ok; else bad "socket parity: $(cat "$WORK/sock.log")"; fi

echo "== malformed AIR payloads rejected without a signal =="
# Hand-crafted AIR1 streams: unknown opcode, out-of-range constant index,
# and a define node with no expression. Each must exit nonzero, never by
# signal (status >= 128), with a diagnostic on stderr.
printf 'AIR1\001\000\000\000\001\310\000\000\000\000\000\000\001' > "$WORK/bad-op.air"
printf 'AIR1\001\000\000\000\001\001\000\143\000\000\000\000\001' > "$WORK/bad-con.air"
printf 'AIR1\001\000\000\000\001\004\000\000\000\000\000\000\001' > "$WORK/bad-def.air"
for bad_air in bad-op bad-con bad-def; do
    ./bin/arturo-stub "$WORK/$bad_air.air" >"$WORK/s.out" 2>"$WORK/s.err"; ns=$?
    if [ "$ns" != "0" ] && [ "$ns" -lt 128 ] && [ -s "$WORK/s.err" ]; then ok; else bad "stub accepted or crashed on $bad_air (status $ns)"; fi
done
./bin/arturoc --emit-ir "$WORK/valid.air" "$ROOT/corpus/05_functions.art" "$WORK/valid-bin" >/dev/null 2>&1
(cd "$WORK" && "$CPP/bin/arturo-stub" valid.air > stub.out 2>stub.err); ss=$?
(cd "$WORK" && ./valid-bin > bin.out 2>bin.err)
if [ "$ss" = "0" ] && cmp -s "$WORK/stub.out" "$WORK/bin.out"; then ok; else bad "stub rejected or diverged on valid AIR"; fi

echo "== CLI contract =="
if ./bin/arturoc --help >"$WORK/help.out" 2>&1 && grep -q "direct" "$WORK/help.out"; then ok; else bad "--help output"; fi
if ./bin/arturoc --version >"$WORK/ver.out" 2>&1 && grep -q "^arturoc [0-9]" "$WORK/ver.out"; then ok; else bad "--version output"; fi
printf 'print "hi"\n' > "$WORK/cli.art"
if ./bin/arturoc --bogus "$WORK/cli.art" "$WORK/cli-bin" >/dev/null 2>&1; then bad "unknown option accepted"; elif [ -e "$WORK/cli-bin" ]; then bad "unknown option produced output"; else ok; fi
if ./bin/arturoc --backend=bogus "$WORK/cli.art" "$WORK/cli-bin" >/dev/null 2>&1; then bad "unknown backend accepted"; elif [ -e "$WORK/cli-bin" ]; then bad "unknown backend produced output"; else ok; fi
if ./bin/arturoc "$WORK/does-not-exist.art" "$WORK/cli-bin" >/dev/null 2>&1; then bad "missing input accepted"; else ok; fi

echo "== C backend: shell quoting and intermediate cleanup =="
evil="$WORK/evil';touch pwned;'"
if ./bin/arturoc --backend=c "$WORK/cli.art" "$evil" >"$WORK/c.log" 2>&1 && [ -x "$evil" ]; then
    eout=$("$evil")
    if [ "$eout" = "hi" ] && [ ! -e "$WORK/pwned" ] && [ ! -e ./pwned ] && [ ! -e "$evil.arturoc.c" ] && [ ! -e "$evil.arturoc.o" ]; then ok
    else bad "C backend quoting/cleanup (out=$eout pwned=$([ -e "$WORK/pwned" ] || [ -e ./pwned ] && echo yes))"; fi
else bad "C backend failed on quoted output path: $(cat "$WORK/c.log")"; fi

echo "== nesting depth rejected with a diagnostic, not a crash =="
python3 -c 'import sys; sys.stdout.write("print 1" + " + 1" * 60000 + "\n")' > "$WORK/deep.art"
./bin/arturoc "$WORK/deep.art" "$WORK/deep-bin" >"$WORK/c.log" 2>&1; ds=$?
if [ "$ds" != "0" ] && [ "$ds" -lt 128 ] && grep -qi "too deeply" "$WORK/c.log"; then ok; else bad "deep infix nesting (status $ds): $(cat "$WORK/c.log")"; fi
python3 -c 'import sys; sys.stdout.write("x: " + "[" * 100000 + "]" * 100000 + "\nprint 1\n")' > "$WORK/deep2.art"
./bin/arturoc "$WORK/deep2.art" "$WORK/deep-bin" >"$WORK/c.log" 2>&1; ds=$?
if [ "$ds" != "0" ] && [ "$ds" -lt 128 ] && grep -qi "too deeply" "$WORK/c.log"; then ok; else bad "deep block nesting (status $ds): $(cat "$WORK/c.log")"; fi

echo "== arturoc tests: pass=$pass fail=$fail =="
[ "$fail" -eq 0 ]
