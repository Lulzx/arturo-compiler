# arturoc benchmark results — 2026-10-15

Compiler under test: `cpp26/bin/arturoc` (the C++26 compiler), all three backends
(`fast` default, `c`, `direct`), compared against the pinned Arturo 0.10.0 host
interpreter and the original self-hosted native compiler (`tmp/ncomp`).
Raw data: `/tmp/bench-results.json`; harnesses: `/tmp/bench_arturoc.py`,
`/tmp/bench_ncomp.py` (this session), plus the repo's own `make -C cpp26 bench`,
whose numbers independently agree with the harness below to within ~0.3 ms.

## Environment and method

| Item | Value |
|---|---|
| Machine | Apple M4 Pro, 12 cores, arm64 |
| OS / toolchain | macOS (Darwin 27), Apple clang 21.0.0, `-O2` |
| Host reference | Arturo 0.10.0 (`arturo-bin/arturo`, 10,245,304 B) |
| Timing | Wall clock incl. process spawn; median of 5–15 reps (min/p95/stdev in raw JSON) |
| Parity gate | No timing counted unless stdout + exit status match the host byte for byte |

## Correctness (must pass before any number counts)

- Clean build (`make -C cpp26 clean && make -C cpp26`): **5.47 s** wall.
- `make -C cpp26 test`: **319 passed, 0 failed** in 22.7 s — 112/112 corpus and
  32/32 upstream programs byte-identical to the host on `fast`, `-O0`≡`-O1`
  optimizer equivalence on the corpus, C/direct parity on the subset, compat
  goldens, unsupported-capability rejection, malformed-AIR rejection, CLI contract.
- All 10 `tools/bench/*.art` workloads: parity `ok` on fast, c, and direct.

## Compile time

Corpus aggregate (all 112 owned corpus programs, total wall time):

| Backend | Total | Per program | Programs/s | Failures |
|---|---:|---:|---:|---:|
| fast | 423 ms | **3.78 ms** | **265** | 0 |
| fast `-O0` | 419 ms | 3.74 ms | 268 | 0 |
| c | 6,584 ms | 58.8 ms | 17.0 | 0 |
| direct | 7,437 ms | 66.4 ms | 15.1 | 0 |
| ncomp (original) | 7,276 ms | 65.0 ms | 15.4 | 0 |

So `fast` compiles **~17× faster than ncomp** and ~16–18× faster than the
C-invoking backends (whose ~57 ms floor is `cc` startup, not arturoc). On the
large upstream file `lib.collections.art` the gap widens: fast 10.7 ms vs
c 1,326 ms, direct 1,851 ms, ncomp 770 ms (official `make bench`).
The optimizer is effectively free: `-O1` vs `-O0` differ by <1% in compile time
and by ~20 bytes in output size. `--stats` breakdown on a bench program:
read 0.04 ms, lex 0.02 ms, front-end 0.14 ms, optimizer 0.02 ms, backend
(stub copy + payload write) 1.97 ms of 2.22 ms total — compile time is
process/file I/O, not compilation.

Scaling (synthetic chained-definition programs, fast backend):

| Lines | Source | Compile (median) | Output size | Marginal rate |
|---:|---:|---:|---:|---:|
| 100 | 1.3 KB | 4.18 ms | 493 KB | — |
| 1,000 | 14.8 KB | 5.46 ms | 540 KB | — |
| 5,000 | 82.8 KB | 12.65 ms | 760 KB | — |
| 10,000 | 167.8 KB | 22.11 ms | 1,050 KB | ≈550k lines/s above the ~4 ms floor |

Compile time is linear with a fixed ~4 ms process/copy floor; compiler peak RSS
compiling the 10k-line program was 44.5 MB.

## Generated binary size

| Producer | Size | Notes |
|---|---:|---|
| fast | 488,406–491,416 B (avg 488,884) | prebuilt stub (488,184 B) + AIR payload; payload itself is only ~0.2–3 KB per corpus program |
| c | 445,792 B (constant) | runtime linked, no payload |
| direct | 445,824–462,336 B | grows slightly with lowered code |
| ncomp | avg 374,501 B (33 KB–419 KB) | varies; some programs get much smaller specialized binaries |
| trivial `print "hello"` | fast 488,351 B / direct 445,840 B | size is a fixed runtime floor, ~independent of program |

Every compiled program carries the whole runtime, so size is ~0.45–0.49 MB
regardless of source size until the payload/code exceeds a few KB. `strip`
reduces `arturoc` itself 519 KB → 450 KB; fast-backend outputs cannot be
stripped (Mach-O `__LINKEDIT` does not cover the appended payload).

## Run time (median ms, `tools/bench`, parity-checked)

| Program | Host | fast | c | direct | ncomp | Best speedup vs host |
|---|---:|---:|---:|---:|---:|---:|
| closures | 56.0 | 19.4 | 19.1 | 18.9 | 21.6 | 3.0× |
| dict_insert | 35.8 | 22.7 | 22.3 | 22.2 | 24.0 | 1.6× |
| dict_iter | 55.6 | 27.1 | 21.6 | 21.8 | 23.0 | 2.6× |
| fib | 199.2 | 62.6 | 61.8 | **49.5** | 62.3 | 4.0× |
| loop_sum | 66.1 | 31.6 | 25.6 | 25.9 | 26.4 | 2.6× |
| map_select | 84.9 | 49.8 | 39.6 | 39.7 | 39.7 | 2.1× |
| nested_loops | 60.1 | 28.8 | 28.6 | 28.7 | **4.7** | 2.1× (ncomp 12.7×) |
| sort | 134.1 | 34.4 | 34.6 | 34.0 | 35.4 | 3.9× |
| string_append | 14.5 | 4.8 | 4.8 | 4.6 | 7.7 | 3.1× |
| while_counter | 339.3 | 126.6 | 133.8 | **94.3** | 134.7 | 3.6× |

Geometric-mean speedup vs the host interpreter: **fast 2.42×, c 2.59×,
direct 2.75×** (range 1.6–4.0×). `direct` wins exactly where it is designed to:
recursion (`fib`) and tight loops (`while_counter`). `ncomp`-compiled programs
run at roughly fast/c speed except `nested_loops`, where ncomp's generated C is
an outlier at 4.7 ms — worth investigating if loop codegen is ever revisited.

## Startup, memory, compiler itself

| Metric | Host | fast binary | direct binary |
|---|---:|---:|---:|
| `print "hello"` startup | 5.57 ms | **2.38 ms** | 3.11 ms |
| Peak RSS across bench | 14–44 MB | 2.5–47 MB | — |

Compiled binaries start ~2.3× faster than the interpreter and generally use
less or equal memory; the exceptions (`map_select`, `sort`) are data-dominated
and within ~10% of the host. The compiler itself is small: `arturoc`
518,584 B and `arturo-stub` 488,184 B, vs the 10.2 MB host interpreter and
1.36 MB `ncomp`.

## Findings

1. **The fast backend is the headline**: 3.8 ms/program, 265 programs/s, no C
   compiler invoked, at a 2.42× geometric-mean runtime speedup over the host.
2. **Binary size is the weak metric**: a fixed ~446–491 KB runtime floor for
   even a one-line program. ncomp shows smaller outputs (down to 33 KB) are
   possible when codegen is specialized per program.
3. **Compile cost is I/O-bound, not compiler-bound**: front-end + optimizer are
   ~0.2 ms of the 2.2 ms in-process total; the rest is process spawn and copying
   the 488 KB stub. Batch compilation or a slimmer stub would cut latency most.
4. `string_append` remains the slowest relative workload in absolute terms for
   every native backend (all share `runtime.c`), consistent with
   `tools/bench/BASELINE.md`.
