# arturo-compiler

A compiler for Arturo, written in Arturo, that compiles itself.

No two implementations. One rule table, one dispatch, one mode
flag. The interpreter and the compiler are the same function:

```
eval(env, emit(n)) == eval(env, n)      ; for every node n
```

Semantics are written once — one row per construct, carrying both
its interpret behavior and its emit behavior. Adding a language
feature is adding a row. The donated VM owns the primitives; the
kernel owns structure: scoping, control flow, application.

## Run it

    arturo src/compiler.art <in.art> <out.art>   # emit -> fold -> print
    arturo -c <out.art> && arturo -x <out.art.bcode>

## Test it

    arturo src/tests.art       # differential: host vs kernel vs compiled
    arturo src/smoke.art       # interpret mode
    arturo src/bootstrap.art   # the compiler compiles itself

The harness runs every corpus program three ways — on the real
`arturo`, through the kernel interpreter, and through the emitted
IR rendered back to source and recompiled by the host. All three
agree.

The bootstrap goes one further. Stage 1 (the compiler in `src/`)
emits stage 2, a self-contained compiler that runs on the donated
VM with no access to `src/`. Stage 2 then renders every corpus
program exactly as stage 1 did, and re-emits the compiler itself:
stage 3 is byte-identical to stage 2. That fixpoint is the whole
claim — the compiler reproduces itself.

## Files

    src/front.art     strip, lex, lower — the grouping, pinned to ast.nim
    src/kernel.art    the dual evaluator, plus runIR
    src/ir.art        IR, the printer, constant folding
    src/backend.art   runIR; shell out to -c / -x
    src/tests.art     the differential harness
    corpus/           one program per rule, then compositions

Known limits, stated plainly: the emitted code names builtins as
prefix words (`x * 2` becomes `mul x 2`), so a program that also
binds a variable called `mul` will have the emitted call captured
by it — the compiler needs output hygiene it does not yet have.
The kernel does not model mutation through a path reference
(`pop 'c\stack`) or a nested path assignment (`d\a\b: v`); both
work on the host and through `-c`, so the compiler bootstraps,
but interpret mode diverges on them. Read `SPEC.md` for the full
design and `corpus/m1/RESULTS.md` for the pinned host behavior.
