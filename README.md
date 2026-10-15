# arturo-compiler

A self-hosting compiler for [Arturo](https://arturo-lang.io/) — written in Arturo, compiling itself to a native binary. On the subset it owns, compiled output matches the host interpreter byte for byte.

Requires Arturo `0.10.0` on `PATH` (pinned revision in `corpus/m1/RESULTS.md`; CI fetches the official `v0.10.0` binary).

## Quick start

```sh
make ncomp               # build the native compiler -> tmp/ncomp
./tmp/ncomp in.art out   # compile a program
./out                    # run it — no VM needed
```

Or without the native build:

```sh
arturo src/compiler.art in.art out
```

## How it works

Everything is **one table, one row per construct**:

```arturo
defRule "while" #[
    eval: function [env argv][ evalWhile env argv\0 argv\1 ]
    emit: function [env argv][ #[op: "while" ...] ]
    run:  function [env argv][ ... ]
]
```

`eval` is the meaning, `emit` is the IR it produces, `run` is what the IR does. Dispatchers look up the row — they know no construct by name, so adding a feature means adding a row (see `CONTRIBUTING.md`). Even `call` is a row: the kernel applies closures itself rather than exposing their internals to the host.

The invariant every row must satisfy, checked across four engines (host, kernel, compiled, in-kernel IR):

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

Builtin arities aren't hand-maintained — they're generated into `INTRINSICS` from Arturo's own source (`tools/extract_intrinsics.art`), so there's one copy and nothing to drift.

## Proof and scope

`make verify` is the self-hosting proof: the compiler emits itself to C, links `tmp/ncomp`, and that binary compiles every corpus program to output identical to the host — stdout, stderr, exit status, and filesystem effects, each in an isolated directory.

Current coverage: **112 corpus programs** (one per rule, all four engines must agree), **32 unmodified pinned-upstream programs**, randomized differential tests, and the compiler's own source. `demo/getbuiltins.art` shows a real ecosystem program built end to end (`demo/README.md`).

This is not yet the whole language. Unowned constructs (backlog in `SPEC.md`, readiness gate in `LANGUAGE_SUPPORT.md`) are outside the claim until a corpus program pins them.

**One deliberate exception:** Arturo 0.10.0 mishandles `"n=" ++ 13` (a misleading `print` arity error, or silent empty output). The native runtime uses the useful semantics and prints `n=13`; string-to-string append stays byte-identical. These fixed cases live in `compat/` with their own golden outputs, outside the parity corpus.

## Tests

```sh
make test         # 4-way differential over the corpus
make verify       # native == host, byte for byte (self-hosting)
make check        # coverage + test + verify + upstream + compat +
                  # diagnostics + unsupported + random + negative
```

Individual gates: `make upstream|compat|diagnostics|unsupported|coverage|random|negative|sanitize|bench`. Sanitizers run ASan + UBSan (plus LSan on Linux; unavailable in Apple Clang).

## The C++26 compiler (`cpp26/`)

`arturoc` is a second, optimizing native compiler in C++26. It shares this repo's semantics and `runtime/runtime.c` (plus three additive ABI wrappers), but generates its intrinsic tables (all 395 builtins) from `src/` and uses a compact pooled IR with three backends: `fast` (default — prebuilt stub + IR payload, no C compiler at compile time), `c`, and `direct`.

```sh
make -C cpp26                  # -> cpp26/bin/arturoc
./cpp26/bin/arturoc in.art out
make -C cpp26 test bench
```

It meets the same bar — all 112 corpus and 32 upstream programs byte-identical, optimizer output-equivalent to `-O0` — and compiles in ~4–5 ms per program (vs. 39–238 ms for the compiler above), with compiled programs running ~2–4× faster than the host. Exact claim boundary: `cpp26/CONTRACT.md`; implementation: `cpp26/README.md`.

## Layout

```
src/kernel.art      the rule table, dispatchers, runIR
src/front.art       lex + lower          src/ir.art        IR + constant folding
src/intrinsics.art  GENERATED arities    src/semantics.art non-generated judgements
src/modules.art     import resolution    src/cbackend.art  C emitter
src/compiler.art    entry point          src/backend.art   shells out to -c / -x
runtime/runtime.c   native runtime linked into every binary (+ runtime.h)
config/             per-intrinsic compatibility policy
corpus/  compat/  fixtures/  upstream/   test programs and goldens
tools/              generators, build, and test harnesses
cpp26/  demo/       the C++26 compiler; a real program compiled natively
```

## Docs

- `SPEC.md` — full design and precise proof boundary
- `CONTRIBUTING.md` — how to add a construct
- `LANGUAGE_SUPPORT.md` — production-readiness gate and path to full coverage
