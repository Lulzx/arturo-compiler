# arturoc — an optimizing native Arturo compiler in C++26

A ground-up C++26 implementation of the Arturo compiler, built against the
original self-hosting implementation in the parent directory. It compiles
Arturo source to native executables through a compact IR, with three
backends sharing one semantics. The full semantic contract against the
original — what must agree, and where agreement is proven — is in
[CONTRACT.md](CONTRACT.md).

## Build

    make -C cpp26            # -> bin/arturoc, bin/arturo-stub
    make -C cpp26 test       # differential suite (needs the pinned host:
                             # `arturo` on PATH or ../arturo-bin/arturo)
    make -C cpp26 bench      # compile-time and run-time comparisons

Requires a C++26-capable compiler (`clang++ -std=c++2c` is the default;
override with `CXX=...`) and a C compiler for the runtime and the C/direct
backends. No Arturo installation is needed to build or to compile programs
— only to run the differential tests.

## Use

    ./cpp26/bin/arturoc program.art program        # fast backend (default)
    ./program

    ./cpp26/bin/arturoc --backend=c program.art program       # generated C
    ./cpp26/bin/arturoc --backend=direct program.art program  # direct C
    ./cpp26/bin/arturoc -O0 program.art program               # no optimizer
    ./cpp26/bin/arturoc --emit-ir program.air program.art program
    ./cpp26/bin/arturoc --stats --timing program.art program

## How it works

```
source.art ──imports──▶ resolved source ──lexer──▶ tokens ──grouping──▶ call tree
   ──emit──▶ compact IR ──verify──▶ optimizer ──verify──▶ backend
```

- **Lexer** (`src/lexer.hpp`): the host's token kinds, including verbatim,
  curly (dedented), safe (`««»»`, `«` line) strings, quantities (`30`N`),
  versions (`1.2.3-rc1`), colors, regexes, paths, and symbol glyphs matched
  longest-first like the host.
- **Grouping** (`src/parser.hpp`): the original `front.art` stack machine —
  arity frames over the generated intrinsic table, single-precedence
  right-associative infix, shadowing renames, `->`/`=>` expansion,
  compile-time `alias` registration, attributes, path get/set.
- **Imports** (`src/modules.hpp`): the original `modules.art` resolver —
  compile-time inclusion, `import.lean` dictionaries, `import.only`
  projections, package entries, cycle rejection.
- **Compact IR** (`src/ir.hpp`, `src/ir.cpp`): three flat pools (interned
  strings, deduplicated constants, nodes with pool-index children) and the
  `AIR1` binary form (LEB128 varints; a node costs a few bytes). This is
  the semantic IR: one op per kernel construct.
- **Optimizer** (`src/optimizer.hpp`): constant folding restricted to the
  original's pure-intrinsic allowlist and to cases the folder models
  exactly, branch folding, dead-pure-statement and unreachable-code
  removal, sequence flattening, conservative constant propagation, and
  reachability compaction. No pass reorders or duplicates an effect.
- **Verifier** (`src/verify.hpp`): structural IR checks after emission and
  after optimization.
- **Backends**:
  - `fast` (default): writes a native executable by appending the AIR1
    payload to the prebuilt `arturo-stub`. No C compiler runs, so
    compilation is one linear pass (see `make bench`).
  - `c`: renders the optimized IR as C on the original runtime's builder
    API and compiles it with `cc`.
  - `direct`: lowers proven constructs to plain C — C control flow,
    checked inline integer arithmetic with generic fallback, direct calls
    to binding-stable user functions, runtime-ABI frame bindings — with a
    per-expression `runNode` fallback for everything else.

Execution semantics come from the original `runtime/runtime.c` (shared,
with three additive ABI wrappers: `rt_apply`, `rt_define_local`, `rt_let`),
so the fast backend is the original native semantics by construction and
the other backends are held to it by the differential tests.

## Layout

    src/ir.hpp / ir.cpp      compact IR + AIR1 codec
    src/lexer.hpp            token lexer
    src/parser.hpp           arity-aware grouping -> call tree
    src/emit.hpp             call tree -> compact IR
    src/modules.hpp          compile-time import resolution
    src/optimizer.hpp        IR optimization passes
    src/verify.hpp           IR verifier
    src/emit_c.hpp           C backend (IR builders)
    src/direct_c.hpp         direct C backend (tier 1 lowering)
    src/stub.cpp             fast-backend runtime stub (AIR loader)
    src/main.cpp             driver (backends, stats, timing)
    src/tables.inc           GENERATED intrinsic tables (tools/gen_tables.py)
    tests/                   differential suites, owned lists, benchmarks
    CONTRACT.md              the semantic contract against the original

## Status

112/112 local corpus programs and 32/32 pinned upstream programs match
the pinned host byte for byte on the fast backend, and all of the
original's compat cases match their golden outputs; the C and direct
backends match on the tested subset; the optimizer is output-equivalent
to `-O0` across the corpus. See CONTRACT.md §9 for the precise proof
boundary.

Measured speed (`make bench`, medians on the development machine): the
fast backend compiles in ~4–5 ms per program against 39–238 ms for the
original native compiler, since no C compiler runs at program-compile
time. Compiled programs run roughly 2–4× faster than the host interpreter
across `tools/bench`, with the direct backend fastest on the recursion
(`fib`) and loop (`while_counter`) workloads.
