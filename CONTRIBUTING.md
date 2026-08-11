# Contributing

A compiler for Arturo, written in Arturo, that compiles itself. The whole
language lives in one table, and adding a feature is adding a row. This
document is the short version; `SPEC.md` is the full design.

## Architecture in one paragraph

Every construct is one row in a rule table with three columns:

```arturo
defRule "while" #[ eval: ..., emit: ..., run: ... ]
```

`eval` is what the construct means interpreted from source, `emit` is the IR
it produces, `run` is what that IR does when executed. The dispatchers know
no construct by name; they look up the row and pick a column. The invariant
is that the three agree:

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

## Adding a construct

1. Write the corpus program in `corpus/NN_name.art`. It is both the spec and
   the test; every corpus program must run unchanged on the host.
2. Pin the expected behavior from the host: `arturo --no-color corpus/NN_name.art`.
3. Add the row to the rule table in `src/kernel.art` (eval + run columns).
4. Add the `emit` column so the construct lowers to IR.
5. If it needs a new IR node, add it in `src/ir.art` and teach the printer.
6. Run `make test`. The four engines (host, kernel, compiled, runIR) must
   all agree on your corpus program.

The three columns take the same `[env argv]`, so `run` and `eval` are the
same construct in the same shape, one in IR and one in source.

## Layout

    src/front.art       lex + lower (grouping, arity, shadowing)
    src/kernel.art      the rule table, its dispatchers, runIR
    src/ir.art          IR + constant folding
    src/backend.art     runIR; shells out to -c / -x
    src/cbackend.art    C emitter
    src/intrinsics.art  GENERATED — builtin arities, from Arturo's own source
    runtime/runtime.c   the C runtime the backend links against
    corpus/             one program per rule
    tools/              build (cbnative) and the self-hosting proof

Regenerate `src/intrinsics.art` when Arturo's builtin signatures change:

    arturo tools/extract_intrinsics.art [path-to-arturo-checkout]

## Verify

    make ncomp      # build the native compiler -> tmp/ncomp
    make test       # 4-way differential, every corpus program
    make verify     # native compiler output == host, byte for byte

`make verify` is the self-hosting claim: it rebuilds `tmp/ncomp` from the
current source, compiles every corpus program to a native binary with it, and
diffs each binary's output against the host interpreter. A change that keeps
both `make test` and `make verify` green has not changed observable behavior.

## Rules of the road

- The compiler only uses constructs it owns. Through the seams it delegates
  to the host, which is the authority; when kernel and host disagree, the
  host wins.
- No hand-written backend. `runtime.c` is the donated runtime, not a second
  implementation of the language; the semantics live in the rule table.
- Facts go in corpus programs and tests, not prose. A pinned behavior without
  a passing test is not pinned.
